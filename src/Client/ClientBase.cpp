#include <Client/ClientBase.h>
#include <Client/LineReader.h>
#include <Client/ClientBaseHelpers.h>
#include <Client/TestHint.h>
#include <Client/InternalTextLogs.h>
#include <Client/TestTags.h>

#include <base/argsToConfig.h>
#include <base/safeExit.h>
#include <Core/Block.h>
#include <Core/BaseSettingsProgramOptions.h>
#include <Core/Protocol.h>
#include <Common/DateLUT.h>
#include <Common/MemoryTracker.h>
#include <Common/scope_guard_safe.h>
#include <Common/Exception.h>
#include <Common/getNumberOfPhysicalCPUCores.h>
#include <Common/typeid_cast.h>
#include <Common/TerminalSize.h>
#include <Common/clearPasswordFromCommandLine.h>
#include <Common/StringUtils.h>
#include <Common/filesystemHelpers.h>
#include <Common/NetException.h>
#include <Common/tryGetFileNameByFileDescriptor.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <Formats/FormatFactory.h>

#include <Parsers/parseQuery.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/formatAST.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTCreateFunctionQuery.h>
#include <Parsers/Access/ASTCreateUserQuery.h>
#include <Parsers/Access/ASTAuthenticationData.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSetQuery.h>
#include <Parsers/ASTUseQuery.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTQueryWithOutput.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTColumnDeclaration.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/Kusto/ParserKQLStatement.h>
#include <Parsers/PRQL/ParserPRQLQuery.h>
#include <Parsers/Kusto/parseKQLQuery.h>

#include <Processors/Formats/Impl/NullFormat.h>
#include <Processors/Formats/IInputFormat.h>
#include <Processors/Formats/IOutputFormat.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/BuildQueryPipelineSettings.h>
#include <Processors/QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>
#include <Processors/Executors/PullingAsyncPipelineExecutor.h>
#include <Processors/Transforms/AddingDefaultsTransform.h>
#include <QueryPipeline/QueryPipeline.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Interpreters/ReplaceQueryParameterVisitor.h>
#include <Interpreters/ProfileEventsExt.h>
#include <IO/WriteBufferFromOStream.h>
#include <IO/WriteBufferFromFileDescriptor.h>
#include <IO/CompressionMethod.h>
#include <IO/ForkWriteBuffer.h>

#include <Access/AccessControl.h>
#include <Storages/ColumnsDescription.h>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <iostream>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <unordered_map>

#include <Common/config_version.h>
#include "config.h"

namespace fs = std::filesystem;
using namespace std::literals;


namespace CurrentMetrics
{
    extern const Metric MemoryTracking;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int DEADLOCK_AVOIDED;
    extern const int CLIENT_OUTPUT_FORMAT_SPECIFIED;
    extern const int UNKNOWN_PACKET_FROM_SERVER;
    extern const int NO_DATA_TO_INSERT;
    extern const int UNEXPECTED_PACKET_FROM_SERVER;
    extern const int INVALID_USAGE_OF_INPUT;
    extern const int CANNOT_SET_SIGNAL_HANDLER;
    extern const int UNRECOGNIZED_ARGUMENTS;
    extern const int LOGICAL_ERROR;
    extern const int CANNOT_OPEN_FILE;
    extern const int FILE_ALREADY_EXISTS;
    extern const int USER_SESSION_LIMIT_EXCEEDED;
    extern const int NOT_IMPLEMENTED;
    extern const int CANNOT_READ_FROM_FILE_DESCRIPTOR;
}

}

namespace ProfileEvents
{
    extern const Event UserTimeMicroseconds;
    extern const Event SystemTimeMicroseconds;
}

namespace
{
constexpr UInt64 THREAD_GROUP_ID = 0;
}

namespace DB
{

ProgressOption toProgressOption(std::string progress)
{
    boost::to_upper(progress);

    if (progress == "OFF" || progress == "FALSE" || progress == "0" || progress == "NO")
        return ProgressOption::OFF;
    if (progress == "TTY" || progress == "ON" || progress == "TRUE" || progress == "1" || progress == "YES")
        return ProgressOption::TTY;
    if (progress == "ERR")
        return ProgressOption::ERR;
    if (progress == "DEFAULT")
        return ProgressOption::DEFAULT;

    throw boost::program_options::validation_error(boost::program_options::validation_error::invalid_option_value);
}

std::istream& operator>> (std::istream & in, ProgressOption & progress)
{
    std::string token;
    in >> token;
    progress = toProgressOption(token);
    return in;
}

static ClientInfo::QueryKind parseQueryKind(const String & query_kind)
{
    if (query_kind == "initial_query")
        return ClientInfo::QueryKind::INITIAL_QUERY;
    if (query_kind == "secondary_query")
        return ClientInfo::QueryKind::SECONDARY_QUERY;
    if (query_kind == "no_query")
        return ClientInfo::QueryKind::NO_QUERY;
    throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unknown query kind {}", query_kind);
}

static void incrementProfileEventsBlock(Block & dst, const Block & src)
{
    if (!dst)
    {
        dst = src.cloneEmpty();
    }

    assertBlocksHaveEqualStructure(src, dst, "ProfileEvents");

    std::unordered_map<String, size_t> name_pos;
    for (size_t i = 0; i < dst.columns(); ++i)
        name_pos[dst.getByPosition(i).name] = i;

    size_t dst_rows = dst.rows();
    MutableColumns mutable_columns = dst.mutateColumns();

    auto & dst_column_host_name = typeid_cast<ColumnString &>(*mutable_columns[name_pos["host_name"]]);
    auto & dst_array_current_time = typeid_cast<ColumnUInt32 &>(*mutable_columns[name_pos["current_time"]]).getData();
    auto & dst_array_type = typeid_cast<ColumnInt8 &>(*mutable_columns[name_pos["type"]]).getData();
    auto & dst_column_name = typeid_cast<ColumnString &>(*mutable_columns[name_pos["name"]]);
    auto & dst_array_value = typeid_cast<ColumnInt64 &>(*mutable_columns[name_pos["value"]]).getData();

    const auto & src_column_host_name = typeid_cast<const ColumnString &>(*src.getByName("host_name").column);
    const auto & src_array_current_time = typeid_cast<const ColumnUInt32 &>(*src.getByName("current_time").column).getData();
    const auto & src_array_thread_id = typeid_cast<const ColumnUInt64 &>(*src.getByName("thread_id").column).getData();
    const auto & src_column_name = typeid_cast<const ColumnString &>(*src.getByName("name").column);
    const auto & src_array_value = typeid_cast<const ColumnInt64 &>(*src.getByName("value").column).getData();

    struct Id
    {
        StringRef name;
        StringRef host_name;

        bool operator<(const Id & rhs) const
        {
            return std::tie(name, host_name)
                 < std::tie(rhs.name, rhs.host_name);
        }
    };
    std::map<Id, UInt64> rows_by_name;

    for (size_t src_row = 0; src_row < src.rows(); ++src_row)
    {
        /// Filter out threads stats, use stats from thread group
        /// Exactly stats from thread group is stored to the table system.query_log
        /// The stats from threads are less useful.
        /// They take more records, they need to be combined,
        /// there even could be several records from one thread.
        /// Server doesn't send it any more to the clients, so this code left for compatible
        auto thread_id = src_array_thread_id[src_row];
        if (thread_id != THREAD_GROUP_ID)
            continue;

        Id id{
            src_column_name.getDataAt(src_row),
            src_column_host_name.getDataAt(src_row),
        };
        rows_by_name[id] = src_row;
    }

    /// Merge src into dst.
    for (size_t dst_row = 0; dst_row < dst_rows; ++dst_row)
    {
        Id id{
            dst_column_name.getDataAt(dst_row),
            dst_column_host_name.getDataAt(dst_row),
        };

        if (auto it = rows_by_name.find(id); it != rows_by_name.end())
        {
            size_t src_row = it->second;

            dst_array_current_time[dst_row] = src_array_current_time[src_row];

            switch (static_cast<ProfileEvents::Type>(dst_array_type[dst_row]))
            {
                case ProfileEvents::Type::INCREMENT:
                    dst_array_value[dst_row] += src_array_value[src_row];
                    break;
                case ProfileEvents::Type::GAUGE:
                    dst_array_value[dst_row] = src_array_value[src_row];
                    break;
            }

            rows_by_name.erase(it);
        }
    }

    /// Copy rows from src that dst does not contains.
    for (const auto & [id, pos] : rows_by_name)
    {
        for (size_t col = 0; col < src.columns(); ++col)
        {
            mutable_columns[col]->insert((*src.getByPosition(col).column)[pos]);
        }
    }

    dst.setColumns(std::move(mutable_columns));
}


std::atomic<Int32> exit_after_signals = 0;

class QueryInterruptHandler : private boost::noncopyable
{
public:
    /// Store how much interrupt signals can be before stopping the query
    /// by default stop after the first interrupt signal.
    static void start(Int32 signals_before_stop = 1) { exit_after_signals.store(signals_before_stop); }

    /// Set value not greater then 0 to mark the query as stopped.
    static void stop() { exit_after_signals.store(0); }

    /// Return true if the query was stopped.
    /// Query was stopped if it received at least "signals_before_stop" interrupt signals.
    static bool try_stop() { return exit_after_signals.fetch_sub(1) <= 0; }
    static bool cancelled() { return exit_after_signals.load() <= 0; }

    /// Return how much interrupt signals remain before stop.
    static Int32 cancelled_status() { return exit_after_signals.load(); }
};

/// This signal handler is set for SIGINT and SIGQUIT.
void interruptSignalHandler(int signum)
{
    if (QueryInterruptHandler::try_stop())
        safeExit(128 + signum);
}


/// To cancel the query on local format error.
class LocalFormatError : public DB::Exception
{
public:
    using Exception::Exception;
};


ClientBase::~ClientBase() = default;
ClientBase::ClientBase() = default;


void ClientBase::setupSignalHandler()
{
    QueryInterruptHandler::stop();

    struct sigaction new_act;
    memset(&new_act, 0, sizeof(new_act));

    new_act.sa_handler = interruptSignalHandler;
    new_act.sa_flags = 0;

#if defined(OS_DARWIN)
    sigemptyset(&new_act.sa_mask);
#else
    if (sigemptyset(&new_act.sa_mask))
        throw ErrnoException(ErrorCodes::CANNOT_SET_SIGNAL_HANDLER, "Cannot set signal handler");
#endif

    if (sigaction(SIGINT, &new_act, nullptr))
        throw ErrnoException(ErrorCodes::CANNOT_SET_SIGNAL_HANDLER, "Cannot set signal handler");

    if (sigaction(SIGQUIT, &new_act, nullptr))
        throw ErrnoException(ErrorCodes::CANNOT_SET_SIGNAL_HANDLER, "Cannot set signal handler");
}


ASTPtr ClientBase::parseQuery(const char *& pos, const char * end, const Settings & settings, bool allow_multi_statements, bool is_interactive, bool ignore_error)
{
    std::unique_ptr<IParserBase> parser;
    ASTPtr res;

    size_t max_length = 0;

    if (!allow_multi_statements)
        max_length = settings.max_query_size;

    const Dialect & dialect = settings.dialect;

    if (dialect == Dialect::kusto)
        parser = std::make_unique<ParserKQLStatement>(end, settings.allow_settings_after_format_in_insert);
    else if (dialect == Dialect::prql)
        parser = std::make_unique<ParserPRQLQuery>(max_length, settings.max_parser_depth, settings.max_parser_backtracks);
    else
        parser = std::make_unique<ParserQuery>(end, settings.allow_settings_after_format_in_insert);

    if (is_interactive || ignore_error)
    {
        String message;
        if (dialect == Dialect::kusto)
            res = tryParseKQLQuery(*parser, pos, end, message, true, "", allow_multi_statements, max_length, settings.max_parser_depth, settings.max_parser_backtracks, true);
        else
            res = tryParseQuery(*parser, pos, end, message, true, "", allow_multi_statements, max_length, settings.max_parser_depth, settings.max_parser_backtracks, true);

        if (!res)
        {
            std::cerr << std::endl << message << std::endl << std::endl;
            return nullptr;
        }
    }
    else
    {
        if (dialect == Dialect::kusto)
            res = parseKQLQueryAndMovePosition(*parser, pos, end, "", allow_multi_statements, max_length, settings.max_parser_depth, settings.max_parser_backtracks);
        else
            res = parseQueryAndMovePosition(*parser, pos, end, "", allow_multi_statements, max_length, settings.max_parser_depth, settings.max_parser_backtracks);
    }

    if (is_interactive)
    {
        std::cout << std::endl;
        WriteBufferFromOStream res_buf(std::cout, 4096);
        formatAST(*res, res_buf);
        res_buf.finalize();
        std::cout << std::endl << std::endl;
    }

    return res;
}


/// Consumes trailing semicolons and tries to consume the same-line trailing comment.
void ClientBase::adjustQueryEnd(const char *& this_query_end, const char * all_queries_end, uint32_t max_parser_depth, uint32_t max_parser_backtracks)
{
    // We have to skip the trailing semicolon that might be left
    // after VALUES parsing or just after a normal semicolon-terminated query.
    Tokens after_query_tokens(this_query_end, all_queries_end);
    IParser::Pos after_query_iterator(after_query_tokens, max_parser_depth, max_parser_backtracks);
    while (after_query_iterator.isValid() && after_query_iterator->type == TokenType::Semicolon)
    {
        this_query_end = after_query_iterator->end;
        ++after_query_iterator;
    }

    // Now we have to do some extra work to add the trailing
    // same-line comment to the query, but preserve the leading
    // comments of the next query. The trailing comment is important
    // because the test hints are usually written this way, e.g.:
    // select nonexistent_column; -- { serverError 12345 }.
    // The token iterator skips comments and whitespace, so we have
    // to find the newline in the string manually. If it's earlier
    // than the next significant token, it means that the text before
    // newline is some trailing whitespace or comment, and we should
    // add it to our query. There are also several special cases
    // that are described below.
    const auto * newline = find_first_symbols<'\n'>(this_query_end, all_queries_end);
    const char * next_query_begin = after_query_iterator->begin;

    // We include the entire line if the next query starts after
    // it. This is a generic case of trailing in-line comment.
    // The "equals" condition is for case of end of input (they both equal
    // all_queries_end);
    if (newline <= next_query_begin)
    {
        assert(newline >= this_query_end);
        this_query_end = newline;
    }
    else
    {
        // Many queries on one line, can't do anything. By the way, this
        // syntax is probably going to work as expected:
        // select nonexistent /* { serverError 12345 } */; select 1
    }
}


/// Convert external tables to ExternalTableData and send them using the connection.
void ClientBase::sendExternalTables(ASTPtr parsed_query)
{
    const auto * select = parsed_query->as<ASTSelectWithUnionQuery>();
    if (!select && !external_tables.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "External tables could be sent only with select query");

    std::vector<ExternalTableDataPtr> data;
    for (auto & table : external_tables)
        data.emplace_back(table.getData(global_context));

    connection->sendExternalTablesData(data);
}


void ClientBase::onData(Block & block, ASTPtr parsed_query)
{
    if (!block)
        return;

    processed_rows += block.rows();
    /// Even if all blocks are empty, we still need to initialize the output stream to write empty resultset.
    initOutputFormat(block, parsed_query);

    /// The header block containing zero rows was used to initialize
    /// output_format, do not output it.
    /// Also do not output too much data if we're fuzzing.
    if (block.rows() == 0 || (query_fuzzer_runs != 0 && processed_rows >= 100))
        return;

    /// If results are written INTO OUTFILE, we can avoid clearing progress to avoid flicker.
    if (need_render_progress && tty_buf && (!select_into_file || select_into_file_and_stdout))
        progress_indication.clearProgressOutput(*tty_buf);

    try
    {
        output_format->write(materializeBlock(block));
        written_first_block = true;
    }
    catch (const Exception &)
    {
        /// Catch client errors like NO_ROW_DELIMITER
        throw LocalFormatError(getCurrentExceptionMessageAndPattern(print_stack_trace), getCurrentExceptionCode());
    }

    /// Received data block is immediately displayed to the user.
    output_format->flush();

    /// Restore progress bar after data block.
    if (need_render_progress && tty_buf)
    {
        if (select_into_file && !select_into_file_and_stdout)
            std::cerr << "\r";
        progress_indication.writeProgress(*tty_buf);
    }
}


void ClientBase::onLogData(Block & block)
{
    initLogsOutputStream();
    if (need_render_progress && tty_buf)
        progress_indication.clearProgressOutput(*tty_buf);
    logs_out_stream->writeLogs(block);
    logs_out_stream->flush();
}


void ClientBase::onTotals(Block & block, ASTPtr parsed_query)
{
    initOutputFormat(block, parsed_query);
    output_format->setTotals(materializeBlock(block));
}


void ClientBase::onExtremes(Block & block, ASTPtr parsed_query)
{
    initOutputFormat(block, parsed_query);
    output_format->setExtremes(materializeBlock(block));
}


void ClientBase::onReceiveExceptionFromServer(std::unique_ptr<Exception> && e)
{
    have_error = true;
    server_exception = std::move(e);
    resetOutput();
}


void ClientBase::onProfileInfo(const ProfileInfo & profile_info)
{
    if (profile_info.hasAppliedLimit() && output_format)
        output_format->setRowsBeforeLimit(profile_info.getRowsBeforeLimit());
}


void ClientBase::initOutputFormat(const Block & block, ASTPtr parsed_query)
try
{
    if (!output_format)
    {
        /// Ignore all results when fuzzing as they can be huge.
        if (query_fuzzer_runs)
        {
            output_format = std::make_shared<NullOutputFormat>(block);
            return;
        }

        WriteBuffer * out_buf = nullptr;
        if (!pager.empty())
        {
            if (SIG_ERR == signal(SIGPIPE, SIG_IGN))
                throw ErrnoException(ErrorCodes::CANNOT_SET_SIGNAL_HANDLER, "Cannot set signal handler for SIGPIPE");
            /// We need to reset signals that had been installed in the
            /// setupSignalHandler() since terminal will send signals to both
            /// processes and so signals will be delivered to the
            /// clickhouse-client/local as well, which will be terminated when
            /// signal will be delivered second time.
            if (SIG_ERR == signal(SIGINT, SIG_IGN))
                throw ErrnoException(ErrorCodes::CANNOT_SET_SIGNAL_HANDLER, "Cannot set signal handler for SIGINT");
            if (SIG_ERR == signal(SIGQUIT, SIG_IGN))
                throw ErrnoException(ErrorCodes::CANNOT_SET_SIGNAL_HANDLER, "Cannot set signal handler for SIGQUIT");

            ShellCommand::Config config(pager);
            config.pipe_stdin_only = true;
            pager_cmd = ShellCommand::execute(config);
            out_buf = &pager_cmd->in;
        }
        else
        {
            out_buf = &std_out;
        }

        String current_format = default_output_format;

        select_into_file = false;
        select_into_file_and_stdout = false;
        /// The query can specify output format or output file.
        if (const auto * query_with_output = dynamic_cast<const ASTQueryWithOutput *>(parsed_query.get()))
        {
            String out_file;
            if (query_with_output->out_file)
            {
                select_into_file = true;

                const auto & out_file_node = query_with_output->out_file->as<ASTLiteral &>();
                out_file = out_file_node.value.safeGet<std::string>();

                std::string compression_method_string;

                if (query_with_output->compression)
                {
                    const auto & compression_method_node = query_with_output->compression->as<ASTLiteral &>();
                    compression_method_string = compression_method_node.value.safeGet<std::string>();
                }

                CompressionMethod compression_method = chooseCompressionMethod(out_file, compression_method_string);
                UInt64 compression_level = 3;

                if (query_with_output->compression_level)
                {
                    const auto & compression_level_node = query_with_output->compression_level->as<ASTLiteral &>();
                    compression_level_node.value.tryGet<UInt64>(compression_level);
                }

                auto flags = O_WRONLY | O_EXCL;

                auto file_exists = fs::exists(out_file);
                if (file_exists && query_with_output->is_outfile_append)
                    flags |= O_APPEND;
                else if (file_exists && query_with_output->is_outfile_truncate)
                    flags |= O_TRUNC;
                else
                    flags |= O_CREAT;

                out_file_buf = wrapWriteBufferWithCompressionMethod(
                    std::make_unique<WriteBufferFromFile>(out_file, DBMS_DEFAULT_BUFFER_SIZE, flags),
                    compression_method,
                    static_cast<int>(compression_level)
                );

                if (query_with_output->is_into_outfile_with_stdout)
                {
                    select_into_file_and_stdout = true;
                    out_file_buf = std::make_unique<ForkWriteBuffer>(std::vector<WriteBufferPtr>{std::move(out_file_buf),
                            std::make_shared<WriteBufferFromFileDescriptor>(STDOUT_FILENO)});
                }

                // We are writing to file, so default format is the same as in non-interactive mode.
                if (is_interactive && is_default_format)
                    current_format = "TabSeparated";
            }
            if (query_with_output->format != nullptr)
            {
                if (has_vertical_output_suffix)
                    throw Exception(ErrorCodes::CLIENT_OUTPUT_FORMAT_SPECIFIED, "Output format already specified");
                const auto & id = query_with_output->format->as<ASTIdentifier &>();
                current_format = id.name();
            }
            else if (query_with_output->out_file)
            {
                auto format_name = FormatFactory::instance().tryGetFormatFromFileName(out_file);
                if (format_name)
                    current_format = *format_name;
            }
        }

        if (has_vertical_output_suffix)
            current_format = "Vertical";

        bool logs_into_stdout = server_logs_file == "-";
        bool extras_into_stdout = need_render_progress || logs_into_stdout;
        bool select_only_into_file = select_into_file && !select_into_file_and_stdout;

        if (!out_file_buf && default_output_compression_method != CompressionMethod::None)
            out_file_buf = wrapWriteBufferWithCompressionMethod(out_buf, default_output_compression_method, 3, 0);

        /// It is not clear how to write progress and logs
        /// intermixed with data with parallel formatting.
        /// It may increase code complexity significantly.
        if (!extras_into_stdout || select_only_into_file)
            output_format = global_context->getOutputFormatParallelIfPossible(
                current_format, out_file_buf ? *out_file_buf : *out_buf, block);
        else
            output_format = global_context->getOutputFormat(
                current_format, out_file_buf ? *out_file_buf : *out_buf, block);

        output_format->setAutoFlush();
    }
}
catch (...)
{
    throw LocalFormatError(getCurrentExceptionMessageAndPattern(print_stack_trace), getCurrentExceptionCode());
}


void ClientBase::initLogsOutputStream()
{
    if (!logs_out_stream)
    {
        WriteBuffer * wb = out_logs_buf.get();

        bool color_logs = false;
        if (!out_logs_buf)
        {
            if (server_logs_file.empty())
            {
                /// Use stderr by default
                out_logs_buf = std::make_unique<WriteBufferFromFileDescriptor>(STDERR_FILENO);
                wb = out_logs_buf.get();
                color_logs = stderr_is_a_tty;
            }
            else if (server_logs_file == "-")
            {
                /// Use stdout if --server_logs_file=- specified
                wb = &std_out;
                color_logs = stdout_is_a_tty;
            }
            else
            {
                out_logs_buf
                    = std::make_unique<WriteBufferFromFile>(server_logs_file, DBMS_DEFAULT_BUFFER_SIZE, O_WRONLY | O_APPEND | O_CREAT);
                wb = out_logs_buf.get();
            }
        }

        logs_out_stream = std::make_unique<InternalTextLogs>(*wb, color_logs);
    }
}

void ClientBase::adjustSettings()
{
    Settings settings = global_context->getSettings();

    /// NOTE: Do not forget to set changed=false to avoid sending it to the server (to avoid breakage read only profiles)

    /// In case of multi-query we allow data after semicolon since it will be
    /// parsed by the client and interpreted as new query
    if (is_multiquery && !global_context->getSettingsRef().input_format_values_allow_data_after_semicolon.changed)
    {
        settings.input_format_values_allow_data_after_semicolon = true;
        settings.input_format_values_allow_data_after_semicolon.changed = false;
    }

    /// Do not limit pretty format output in case of --pager specified or in case of stdout is not a tty.
    if (!pager.empty() || !stdout_is_a_tty)
    {
        if (!global_context->getSettingsRef().output_format_pretty_max_rows.changed)
        {
            settings.output_format_pretty_max_rows = std::numeric_limits<UInt64>::max();
            settings.output_format_pretty_max_rows.changed = false;
        }

        if (!global_context->getSettingsRef().output_format_pretty_max_value_width.changed)
        {
            settings.output_format_pretty_max_value_width = std::numeric_limits<UInt64>::max();
            settings.output_format_pretty_max_value_width.changed = false;
        }
    }

    global_context->setSettings(settings);
}

bool ClientBase::isRegularFile(int fd)
{
    struct stat file_stat;
    return fstat(fd, &file_stat) == 0 && S_ISREG(file_stat.st_mode);
}

void ClientBase::setDefaultFormatsAndCompressionFromConfiguration()
{
    if (config().has("output-format"))
    {
        default_output_format = config().getString("output-format");
        is_default_format = false;
    }
    else if (config().has("format"))
    {
        default_output_format = config().getString("format");
        is_default_format = false;
    }
    else if (config().has("vertical"))
    {
        default_output_format = "Vertical";
        is_default_format = false;
    }
    else if (isRegularFile(STDOUT_FILENO))
    {
        std::optional<String> format_from_file_name = FormatFactory::instance().tryGetFormatFromFileDescriptor(STDOUT_FILENO);
        if (format_from_file_name)
            default_output_format = *format_from_file_name;
        else
            default_output_format = "TSV";

        std::optional<String> file_name = tryGetFileNameFromFileDescriptor(STDOUT_FILENO);
        if (file_name)
            default_output_compression_method = chooseCompressionMethod(*file_name, "");
    }
    else if (is_interactive)
    {
        default_output_format = "PrettyCompact";
    }
    else
    {
        default_output_format = "TSV";
    }

    if (config().has("input-format"))
    {
        default_input_format = config().getString("input-format");
    }
    else if (config().has("format"))
    {
        default_input_format = config().getString("format");
    }
    else if (config().getString("table-file", "-") != "-")
    {
        auto file_name = config().getString("table-file");
        std::optional<String> format_from_file_name = FormatFactory::instance().tryGetFormatFromFileName(file_name);
        if (format_from_file_name)
            default_input_format = *format_from_file_name;
        else
            default_input_format = "TSV";
    }
    else
    {
        std::optional<String> format_from_file_name = FormatFactory::instance().tryGetFormatFromFileDescriptor(STDIN_FILENO);
        if (format_from_file_name)
            default_input_format = *format_from_file_name;
        else
            default_input_format = "TSV";
    }

    format_max_block_size = config().getUInt64("format_max_block_size",
        global_context->getSettingsRef().max_block_size);

    /// Setting value from cmd arg overrides one from config
    if (global_context->getSettingsRef().max_insert_block_size.changed)
    {
        insert_format_max_block_size = global_context->getSettingsRef().max_insert_block_size;
    }
    else
    {
        insert_format_max_block_size = config().getUInt64("insert_format_max_block_size",
            global_context->getSettingsRef().max_insert_block_size);
    }
}

void ClientBase::initTTYBuffer(ProgressOption progress)
{
    if (tty_buf)
        return;

    if (progress == ProgressOption::OFF || (!is_interactive && progress == ProgressOption::DEFAULT))
    {
         need_render_progress = false;
         return;
    }

    static constexpr auto tty_file_name = "/dev/tty";

    /// Output all progress bar commands to terminal at once to avoid flicker.
    /// This size is usually greater than the window size.
    static constexpr size_t buf_size = 1024;

    if (is_interactive || progress == ProgressOption::TTY)
    {
        std::error_code ec;
        std::filesystem::file_status tty = std::filesystem::status(tty_file_name, ec);

        if (!ec && exists(tty) && is_character_file(tty)
            && (tty.permissions() & std::filesystem::perms::others_write) != std::filesystem::perms::none)
        {
            try
            {
                tty_buf = std::make_unique<WriteBufferFromFile>(tty_file_name, buf_size);

                /// It is possible that the terminal file has writeable permissions
                /// but we cannot write anything there. Check it with invisible character.
                tty_buf->write('\0');
                tty_buf->next();

                return;
            }
            catch (const Exception & e)
            {
                if (tty_buf)
                    tty_buf.reset();

                if (e.code() != ErrorCodes::CANNOT_OPEN_FILE)
                    throw;

                /// It is normal if file exists, indicated as writeable but still cannot be opened.
                /// Fallback to other options.
            }
        }
    }

    if (stderr_is_a_tty || progress == ProgressOption::ERR)
    {
        tty_buf = std::make_unique<WriteBufferFromFileDescriptor>(STDERR_FILENO, buf_size);
    }
    else
        need_render_progress = false;
}

void ClientBase::updateSuggest(const ASTPtr & ast)
{
    std::vector<std::string> new_words;

    if (auto * create = ast->as<ASTCreateQuery>())
    {
        if (create->database)
            new_words.push_back(create->getDatabase());
        new_words.push_back(create->getTable());

        if (create->columns_list && create->columns_list->columns)
        {
            for (const auto & elem : create->columns_list->columns->children)
            {
                if (const auto * column = elem->as<ASTColumnDeclaration>())
                    new_words.push_back(column->name);
            }
        }
    }

    if (const auto * create_function = ast->as<ASTCreateFunctionQuery>())
    {
        new_words.push_back(create_function->getFunctionName());
    }

    if (!new_words.empty())
        suggest->addWords(std::move(new_words));
}

bool ClientBase::isSyncInsertWithData(const ASTInsertQuery & insert_query, const ContextPtr & context)
{
    if (!insert_query.data)
        return false;

    auto settings = context->getSettings();
    if (insert_query.settings_ast)
        settings.applyChanges(insert_query.settings_ast->as<ASTSetQuery>()->changes);

    return !settings.async_insert;
}

void ClientBase::processTextAsSingleQuery(const String & full_query)
{
    /// Some parts of a query (result output and formatting) are executed
    /// client-side. Thus we need to parse the query.
    const char * begin = full_query.data();
    auto parsed_query = parseQuery(begin, begin + full_query.size(),
        global_context->getSettingsRef(),
        /*allow_multi_statements=*/ false,
        is_interactive,
        ignore_error);

    if (!parsed_query)
        return;

    String query_to_execute;

    /// Query will be parsed before checking the result because error does not
    /// always means a problem, i.e. if table already exists, and it is no a
    /// huge problem if suggestion will be added even on error, since this is
    /// just suggestion.
    ///
    /// Do not update suggest, until suggestion will be ready
    /// (this will avoid extra complexity)
    if (suggest)
        updateSuggest(parsed_query);

    /// An INSERT query may have the data that follows query text.
    /// Send part of the query without data, because data will be sent separately.
    /// But for asynchronous inserts we don't extract data, because it's needed
    /// to be done on server side in that case (for coalescing the data from multiple inserts on server side).
    const auto * insert = parsed_query->as<ASTInsertQuery>();
    if (insert && isSyncInsertWithData(*insert, global_context))
        query_to_execute = full_query.substr(0, insert->data - full_query.data());
    else
        query_to_execute = full_query;

    try
    {
        processParsedSingleQuery(full_query, query_to_execute, parsed_query, echo_queries);
    }
    catch (Exception & e)
    {
        if (!is_interactive)
            e.addMessage("(in query: {})", full_query);
        throw;
    }

    if (have_error)
        processError(full_query);
}

void ClientBase::processOrdinaryQuery(const String & query_to_execute, ASTPtr parsed_query)
{
    auto query = query_to_execute;

    /// Rewrite query only when we have query parameters.
    /// Note that if query is rewritten, comments in query are lost.
    /// But the user often wants to see comments in server logs, query log, processlist, etc.
    /// For recent versions of the server query parameters will be transferred by network and applied on the server side.
    if (!query_parameters.empty()
        && connection->getServerRevision(connection_parameters.timeouts) < DBMS_MIN_PROTOCOL_VERSION_WITH_PARAMETERS)
    {
        /// Replace ASTQueryParameter with ASTLiteral for prepared statements.
        ReplaceQueryParameterVisitor visitor(query_parameters);
        visitor.visit(parsed_query);

        /// Get new query after substitutions.
        if (visitor.getNumberOfReplacedParameters())
            query = serializeAST(*parsed_query);
        chassert(!query.empty());
    }

    if (allow_merge_tree_settings && parsed_query->as<ASTCreateQuery>())
    {
        /// Rewrite query if new settings were added.
        if (addMergeTreeSettings(*parsed_query->as<ASTCreateQuery>()))
        {
            /// Replace query parameters because AST cannot be serialized otherwise.
            if (!query_parameters.empty())
            {
                ReplaceQueryParameterVisitor visitor(query_parameters);
                visitor.visit(parsed_query);
            }

            query = serializeAST(*parsed_query);
        }
    }

    // Run some local checks to make sure queries into output file will work before sending to server.
    if (const auto * query_with_output = dynamic_cast<const ASTQueryWithOutput *>(parsed_query.get()))
    {
        String out_file;
        if (query_with_output->out_file)
        {
            const auto & out_file_node = query_with_output->out_file->as<ASTLiteral &>();
            out_file = out_file_node.value.safeGet<std::string>();

            std::string compression_method_string;

            if (query_with_output->compression)
            {
                const auto & compression_method_node = query_with_output->compression->as<ASTLiteral &>();
                compression_method_string = compression_method_node.value.safeGet<std::string>();
            }

            CompressionMethod compression_method = chooseCompressionMethod(out_file, compression_method_string);
            UInt64 compression_level = 3;

            if (query_with_output->is_outfile_append && query_with_output->is_outfile_truncate)
            {
                throw Exception(
                    ErrorCodes::BAD_ARGUMENTS,
                    "Cannot use INTO OUTFILE with APPEND and TRUNCATE simultaneously.");
            }

            if (query_with_output->is_outfile_append && compression_method != CompressionMethod::None)
            {
                throw Exception(
                    ErrorCodes::BAD_ARGUMENTS,
                    "Cannot append to compressed file. Please use uncompressed file or remove APPEND keyword.");
            }

            if (query_with_output->compression_level)
            {
                const auto & compression_level_node = query_with_output->compression_level->as<ASTLiteral &>();
                bool res = compression_level_node.value.tryGet<UInt64>(compression_level);
                auto range = getCompressionLevelRange(compression_method);

                if (!res || compression_level < range.first || compression_level > range.second)
                    throw Exception(
                        ErrorCodes::BAD_ARGUMENTS,
                        "Invalid compression level, must be positive integer in range {}-{}",
                        range.first,
                        range.second);
            }

            if (fs::exists(out_file))
            {
                if (!query_with_output->is_outfile_append && !query_with_output->is_outfile_truncate)
                {
                    throw Exception(
                        ErrorCodes::FILE_ALREADY_EXISTS,
                        "File {} exists, consider using APPEND or TRUNCATE.",
                        out_file);
                }
            }
        }
    }

    const auto & settings = global_context->getSettingsRef();
    const Int32 signals_before_stop = settings.partial_result_on_first_cancel ? 2 : 1;

    int retries_left = 10;
    while (retries_left)
    {
        try
        {
            QueryInterruptHandler::start(signals_before_stop);
            SCOPE_EXIT({ QueryInterruptHandler::stop(); });

            connection->sendQuery(
                connection_parameters.timeouts,
                query,
                query_parameters,
                global_context->getCurrentQueryId(),
                query_processing_stage,
                &global_context->getSettingsRef(),
                &global_context->getClientInfo(),
                true,
                [&](const Progress & progress) { onProgress(progress); });

            if (send_external_tables)
                sendExternalTables(parsed_query);
            receiveResult(parsed_query, signals_before_stop, settings.partial_result_on_first_cancel);

            break;
        }
        catch (const Exception & e)
        {
            /// Retry when the server said "Client should retry" and no rows
            /// has been received yet.
            if (processed_rows == 0 && e.code() == ErrorCodes::DEADLOCK_AVOIDED && --retries_left)
            {
                std::cerr << "Got a transient error from the server, will"
                        << " retry (" << retries_left << " retries left)";
            }
            else
            {
                throw;
            }
        }
    }
    assert(retries_left > 0);
}


/// Receives and processes packets coming from server.
/// Also checks if query execution should be cancelled.
void ClientBase::receiveResult(ASTPtr parsed_query, Int32 signals_before_stop, bool partial_result_on_first_cancel)
{
    // TODO: get the poll_interval from commandline.
    const auto receive_timeout = connection_parameters.timeouts.receive_timeout;
    constexpr size_t default_poll_interval = 1000000; /// in microseconds
    constexpr size_t min_poll_interval = 5000; /// in microseconds
    const size_t poll_interval
        = std::max(min_poll_interval, std::min<size_t>(receive_timeout.totalMicroseconds(), default_poll_interval));

    bool break_on_timeout = connection->getConnectionType() != IServerConnection::Type::LOCAL;

    std::exception_ptr local_format_error;

    while (true)
    {
        Stopwatch receive_watch(CLOCK_MONOTONIC_COARSE);

        while (true)
        {
            /// Has the Ctrl+C been pressed and thus the query should be cancelled?
            /// If this is the case, inform the server about it and receive the remaining packets
            /// to avoid losing sync.
            if (!cancelled)
            {
                if (partial_result_on_first_cancel && QueryInterruptHandler::cancelled_status() == signals_before_stop - 1)
                {
                    connection->sendCancel();
                    /// First cancel reading request was sent. Next requests will only be with a full cancel
                    partial_result_on_first_cancel = false;
                }
                else if (QueryInterruptHandler::cancelled())
                {
                    cancelQuery();
                }
                else
                {
                    double elapsed = receive_watch.elapsedSeconds();
                    if (break_on_timeout && elapsed > receive_timeout.totalSeconds())
                    {
                        std::cout << "Timeout exceeded while receiving data from server."
                                    << " Waited for " << static_cast<size_t>(elapsed) << " seconds,"
                                    << " timeout is " << receive_timeout.totalSeconds() << " seconds." << std::endl;

                        cancelQuery();
                    }
                }
            }

            /// Poll for changes after a cancellation check, otherwise it never reached
            /// because of progress updates from server.

            if (connection->poll(poll_interval))
                break;
        }

        try
        {
            if (!receiveAndProcessPacket(parsed_query, cancelled))
                break;
        }
        catch (const LocalFormatError &)
        {
            /// Remember the first exception.
            if (!local_format_error)
                local_format_error = std::current_exception();
            connection->sendCancel();
        }
    }

    if (local_format_error)
        std::rethrow_exception(local_format_error);

    if (cancelled && is_interactive)
    {
        std::cout << "Query was cancelled." << std::endl;
        cancelled_printed = true;
    }
}


/// Receive a part of the result, or progress info or an exception and process it.
/// Returns true if one should continue receiving packets.
/// Output of result is suppressed if query was cancelled.
bool ClientBase::receiveAndProcessPacket(ASTPtr parsed_query, bool cancelled_)
{
    Packet packet = connection->receivePacket();

    switch (packet.type)
    {
        case Protocol::Server::PartUUIDs:
            return true;

        case Protocol::Server::Data:
            if (!cancelled_)
                onData(packet.block, parsed_query);
            return true;

        case Protocol::Server::Progress:
            onProgress(packet.progress);
            return true;

        case Protocol::Server::ProfileInfo:
            onProfileInfo(packet.profile_info);
            return true;

        case Protocol::Server::Totals:
            if (!cancelled_)
                onTotals(packet.block, parsed_query);
            return true;

        case Protocol::Server::Extremes:
            if (!cancelled_)
                onExtremes(packet.block, parsed_query);
            return true;

        case Protocol::Server::Exception:
            onReceiveExceptionFromServer(std::move(packet.exception));
            return false;

        case Protocol::Server::Log:
            onLogData(packet.block);
            return true;

        case Protocol::Server::EndOfStream:
            onEndOfStream();
            return false;

        case Protocol::Server::ProfileEvents:
            onProfileEvents(packet.block);
            return true;

        case Protocol::Server::TimezoneUpdate:
            onTimezoneUpdate(packet.server_timezone);
            return true;

        default:
            throw Exception(
                ErrorCodes::UNKNOWN_PACKET_FROM_SERVER, "Unknown packet {} from server {}", packet.type, connection->getDescription());
    }
}


void ClientBase::onProgress(const Progress & value)
{
    if (!progress_indication.updateProgress(value))
    {
        // Just a keep-alive update.
        return;
    }

    if (output_format)
        output_format->onProgress(value);

    if (need_render_progress && tty_buf)
        progress_indication.writeProgress(*tty_buf);
}

void ClientBase::onTimezoneUpdate(const String & tz)
{
    global_context->setSetting("session_timezone", tz);
}


void ClientBase::onEndOfStream()
{
    if (need_render_progress && tty_buf)
        progress_indication.clearProgressOutput(*tty_buf);

    if (output_format)
    {
        /// Do our best to estimate the start of the query so the output format matches the one reported by the server
        bool is_running = false;
        output_format->setStartTime(
            clock_gettime_ns(CLOCK_MONOTONIC) - static_cast<UInt64>(progress_indication.elapsedSeconds() * 1000000000), is_running);

        try
        {
            output_format->finalize();
        }
        catch (...)
        {
            /// Format should be reset to make it work for subsequent query
            /// (otherwise it will throw again in resetOutput())
            output_format.reset();
            throw;
        }
    }

    resetOutput();

    if (is_interactive)
    {
        if (cancelled && !cancelled_printed)
            std::cout << "Query was cancelled." << std::endl;
        else if (!written_first_block)
            std::cout << "Ok." << std::endl;
    }
}


void ClientBase::onProfileEvents(Block & block)
{
    const auto rows = block.rows();
    if (rows == 0)
        return;

    if (getName() == "local" || server_revision >= DBMS_MIN_PROTOCOL_VERSION_WITH_INCREMENTAL_PROFILE_EVENTS)
    {
        const auto & array_thread_id = typeid_cast<const ColumnUInt64 &>(*block.getByName("thread_id").column).getData();
        const auto & names = typeid_cast<const ColumnString &>(*block.getByName("name").column);
        const auto & host_names = typeid_cast<const ColumnString &>(*block.getByName("host_name").column);
        const auto & array_values = typeid_cast<const ColumnInt64 &>(*block.getByName("value").column).getData();

        const auto * user_time_name = ProfileEvents::getName(ProfileEvents::UserTimeMicroseconds);
        const auto * system_time_name = ProfileEvents::getName(ProfileEvents::SystemTimeMicroseconds);

        HostToTimesMap thread_times;
        for (size_t i = 0; i < rows; ++i)
        {
            auto thread_id = array_thread_id[i];
            auto host_name = host_names.getDataAt(i).toString();

            /// In ProfileEvents packets thread id 0 specifies common profiling information
            /// for all threads executing current query on specific host. So instead of summing per thread
            /// consumption it's enough to look for data with thread id 0.
            if (thread_id != THREAD_GROUP_ID)
                continue;

            auto event_name = names.getDataAt(i);
            auto value = array_values[i];

            /// Ignore negative time delta or memory usage just in case.
            if (value < 0)
                continue;

            if (event_name == user_time_name)
                thread_times[host_name].user_ms = value;
            else if (event_name == system_time_name)
                thread_times[host_name].system_ms = value;
            else if (event_name == MemoryTracker::USAGE_EVENT_NAME)
                thread_times[host_name].memory_usage = value;
            else if (event_name == MemoryTracker::PEAK_USAGE_EVENT_NAME)
                thread_times[host_name].peak_memory_usage = value;
        }
        progress_indication.updateThreadEventData(thread_times);

        if (need_render_progress && tty_buf)
            progress_indication.writeProgress(*tty_buf);

        if (profile_events.print)
        {
            if (profile_events.watch.elapsedMilliseconds() >= profile_events.delay_ms)
            {
                /// We need to restart the watch each time we flushed these events
                profile_events.watch.restart();
                initLogsOutputStream();
                if (need_render_progress && tty_buf)
                    progress_indication.clearProgressOutput(*tty_buf);
                logs_out_stream->writeProfileEvents(block);
                logs_out_stream->flush();

                profile_events.last_block = {};
            }
            else
            {
                incrementProfileEventsBlock(profile_events.last_block, block);
            }
        }
    }
}


/// Flush all buffers.
void ClientBase::resetOutput()
{
    /// Order is important: format, compression, file

    if (output_format)
        output_format->finalize();
    output_format.reset();

    logs_out_stream.reset();

    if (out_file_buf)
    {
        out_file_buf->finalize();
        out_file_buf.reset();
    }

    if (pager_cmd)
    {
        pager_cmd->in.close();
        pager_cmd->wait();

        if (SIG_ERR == signal(SIGPIPE, SIG_DFL))
            throw ErrnoException(ErrorCodes::CANNOT_SET_SIGNAL_HANDLER, "Cannot set signal handler for SIGPIPE");
        if (SIG_ERR == signal(SIGINT, SIG_DFL))
            throw ErrnoException(ErrorCodes::CANNOT_SET_SIGNAL_HANDLER, "Cannot set signal handler for SIGINT");
        if (SIG_ERR == signal(SIGQUIT, SIG_DFL))
            throw ErrnoException(ErrorCodes::CANNOT_SET_SIGNAL_HANDLER, "Cannot set signal handler for SIGQUIT");

        setupSignalHandler();
    }
    pager_cmd = nullptr;

    if (out_logs_buf)
    {
        out_logs_buf->finalize();
        out_logs_buf.reset();
    }

    std_out.next();
}


/// Receive the block that serves as an example of the structure of table where data will be inserted.
bool ClientBase::receiveSampleBlock(Block & out, ColumnsDescription & columns_description, ASTPtr parsed_query)
{
    while (true)
    {
        Packet packet = connection->receivePacket();

        switch (packet.type)
        {
            case Protocol::Server::Data:
                out = packet.block;
                return true;

            case Protocol::Server::Exception:
                onReceiveExceptionFromServer(std::move(packet.exception));
                return false;

            case Protocol::Server::Log:
                onLogData(packet.block);
                break;

            case Protocol::Server::TableColumns:
                columns_description = ColumnsDescription::parse(packet.multistring_message[1]);
                return receiveSampleBlock(out, columns_description, parsed_query);

            case Protocol::Server::TimezoneUpdate:
                onTimezoneUpdate(packet.server_timezone);
                break;

            default:
                throw NetException(ErrorCodes::UNEXPECTED_PACKET_FROM_SERVER,
                    "Unexpected packet from server (expected Data, Exception, Log or TimezoneUpdate, got {})",
                    String(Protocol::Server::toString(packet.type)));
        }
    }
}


void ClientBase::setInsertionTable(const ASTInsertQuery & insert_query)
{
    if (!global_context->hasInsertionTable() && insert_query.table)
    {
        String table = insert_query.table->as<ASTIdentifier &>().shortName();
        if (!table.empty())
        {
            String database = insert_query.database ? insert_query.database->as<ASTIdentifier &>().shortName() : "";
            global_context->setInsertionTable(StorageID(database, table));
        }
    }
}


void ClientBase::addMultiquery(std::string_view query, Arguments & common_arguments) const
{
    common_arguments.emplace_back("--multiquery");
    common_arguments.emplace_back("-q");
    common_arguments.emplace_back(query);
}

namespace
{
bool isStdinNotEmptyAndValid(ReadBufferFromFileDescriptor & std_in)
{
    try
    {
        return !std_in.eof();
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::CANNOT_READ_FROM_FILE_DESCRIPTOR)
            return false;
        throw;
    }
}
}


void ClientBase::processInsertQuery(const String & query_to_execute, ASTPtr parsed_query)
{
    auto query = query_to_execute;
    if (!query_parameters.empty()
        && connection->getServerRevision(connection_parameters.timeouts) < DBMS_MIN_PROTOCOL_VERSION_WITH_PARAMETERS)
    {
        /// Replace ASTQueryParameter with ASTLiteral for prepared statements.
        ReplaceQueryParameterVisitor visitor(query_parameters);
        visitor.visit(parsed_query);

        /// Get new query after substitutions.
        if (visitor.getNumberOfReplacedParameters())
            query = serializeAST(*parsed_query);
        chassert(!query.empty());
    }

    /// Process the query that requires transferring data blocks to the server.
    const auto & parsed_insert_query = parsed_query->as<ASTInsertQuery &>();
    if ((!parsed_insert_query.data && !parsed_insert_query.infile) && (is_interactive || (!stdin_is_a_tty && !isStdinNotEmptyAndValid(std_in))))
    {
        const auto & settings = global_context->getSettingsRef();
        if (settings.throw_if_no_data_to_insert)
            throw Exception(ErrorCodes::NO_DATA_TO_INSERT, "No data to insert");
        else
            return;
    }

    QueryInterruptHandler::start();
    SCOPE_EXIT({ QueryInterruptHandler::stop(); });

    connection->sendQuery(
        connection_parameters.timeouts,
        query,
        query_parameters,
        global_context->getCurrentQueryId(),
        query_processing_stage,
        &global_context->getSettingsRef(),
        &global_context->getClientInfo(),
        true,
        [&](const Progress & progress) { onProgress(progress); });

    if (send_external_tables)
        sendExternalTables(parsed_query);

    /// Receive description of table structure.
    Block sample;
    ColumnsDescription columns_description;
    if (receiveSampleBlock(sample, columns_description, parsed_query))
    {
        /// If structure was received (thus, server has not thrown an exception),
        /// send our data with that structure.
        setInsertionTable(parsed_insert_query);

        sendData(sample, columns_description, parsed_query);
        receiveEndOfQuery();
    }
}


void ClientBase::sendData(Block & sample, const ColumnsDescription & columns_description, ASTPtr parsed_query)
{
    /// Get columns description from variable or (if it was empty) create it from sample.
    auto columns_description_for_query = columns_description.empty() ? ColumnsDescription(sample.getNamesAndTypesList()) : columns_description;
    if (columns_description_for_query.empty())
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "Column description is empty and it can't be built from sample from table. "
                        "Cannot execute query.");
    }

    /// If INSERT data must be sent.
    auto * parsed_insert_query = parsed_query->as<ASTInsertQuery>();
    if (!parsed_insert_query)
        return;

    bool have_data_in_stdin = !is_interactive && !stdin_is_a_tty && isStdinNotEmptyAndValid(std_in);

    if (need_render_progress)
    {
        /// Set total_bytes_to_read for current fd.
        FileProgress file_progress(0, std_in.getFileSize());
        progress_indication.updateProgress(Progress(file_progress));

        /// Set callback to be called on file progress.
        if (tty_buf)
            progress_indication.setFileProgressCallback(global_context, *tty_buf);
    }

    /// If data fetched from file (maybe compressed file)
    if (parsed_insert_query->infile)
    {
        /// Get name of this file (path to file)
        const auto & in_file_node = parsed_insert_query->infile->as<ASTLiteral &>();
        const auto in_file = in_file_node.value.safeGet<std::string>();

        std::string compression_method;
        /// Compression method can be specified in query
        if (parsed_insert_query->compression)
        {
            const auto & compression_method_node = parsed_insert_query->compression->as<ASTLiteral &>();
            compression_method = compression_method_node.value.safeGet<std::string>();
        }

        String current_format = parsed_insert_query->format;
        if (current_format.empty())
            current_format = FormatFactory::instance().getFormatFromFileName(in_file);

        /// Create temporary storage file, to support globs and parallel reading
        /// StorageFile doesn't support ephemeral/materialized/alias columns.
        /// We should change ephemeral columns to ordinary and ignore materialized/alias columns.
        ColumnsDescription columns_for_storage_file;
        for (const auto & [name, _] : columns_description_for_query.getInsertable())
        {
            ColumnDescription column = columns_description_for_query.get(name);
            column.default_desc.kind = ColumnDefaultKind::Default;
            columns_for_storage_file.add(std::move(column));
        }

        StorageFile::CommonArguments args{
            WithContext(global_context),
            parsed_insert_query->table_id,
            current_format,
            getFormatSettings(global_context),
            compression_method,
            columns_for_storage_file,
            ConstraintsDescription{},
            String{},
            {},
            String{},
        };
        StoragePtr storage = std::make_shared<StorageFile>(in_file, global_context->getUserFilesPath(), args);
        storage->startup();
        SelectQueryInfo query_info;

        try
        {
            auto metadata = storage->getInMemoryMetadataPtr();
            QueryPlan plan;
            storage->read(
                    plan,
                    sample.getNames(),
                    storage->getStorageSnapshot(metadata, global_context),
                    query_info,
                    global_context,
                    {},
                    global_context->getSettingsRef().max_block_size,
                    getNumberOfPhysicalCPUCores());

            auto builder = plan.buildQueryPipeline(
                QueryPlanOptimizationSettings::fromContext(global_context),
                BuildQueryPipelineSettings::fromContext(global_context));

            QueryPlanResourceHolder resources;
            auto pipe = QueryPipelineBuilder::getPipe(std::move(*builder), resources);

            sendDataFromPipe(
                std::move(pipe),
                parsed_query,
                have_data_in_stdin);
        }
        catch (Exception & e)
        {
            e.addMessage("data for INSERT was parsed from file");
            throw;
        }

        if (have_data_in_stdin && !cancelled)
            sendDataFromStdin(sample, columns_description_for_query, parsed_query);
    }
    else if (parsed_insert_query->data)
    {
        /// Send data contained in the query.
        ReadBufferFromMemory data_in(parsed_insert_query->data, parsed_insert_query->end - parsed_insert_query->data);
        try
        {
            sendDataFrom(data_in, sample, columns_description_for_query, parsed_query, have_data_in_stdin);
            if (have_data_in_stdin && !cancelled)
                sendDataFromStdin(sample, columns_description_for_query, parsed_query);
        }
        catch (Exception & e)
        {
            /// The following query will use data from input
            //      "INSERT INTO data FORMAT TSV\n " < data.csv
            //  And may be pretty hard to debug, so add information about data source to make it easier.
            e.addMessage("data for INSERT was parsed from query");
            throw;
        }
        // Remember where the data ended. We use this info later to determine
        // where the next query begins.
        parsed_insert_query->end = parsed_insert_query->data + data_in.count();
    }
    else if (!is_interactive)
    {
        sendDataFromStdin(sample, columns_description_for_query, parsed_query);
    }
    else
        throw Exception(ErrorCodes::NO_DATA_TO_INSERT, "No data to insert");
}


void ClientBase::sendDataFrom(ReadBuffer & buf, Block & sample, const ColumnsDescription & columns_description, ASTPtr parsed_query, bool have_more_data)
{
    String current_format = "Values";

    /// Data format can be specified in the INSERT query.
    if (const auto * insert = parsed_query->as<ASTInsertQuery>())
    {
        if (!insert->format.empty())
            current_format = insert->format;
    }

    auto source = global_context->getInputFormat(current_format, buf, sample, insert_format_max_block_size);
    Pipe pipe(source);

    if (columns_description.hasDefaults())
    {
        pipe.addSimpleTransform([&](const Block & header)
        {
            return std::make_shared<AddingDefaultsTransform>(header, columns_description, *source, global_context);
        });
    }

    sendDataFromPipe(std::move(pipe), parsed_query, have_more_data);
}

void ClientBase::sendDataFromPipe(Pipe&& pipe, ASTPtr parsed_query, bool have_more_data)
try
{
    QueryPipeline pipeline(std::move(pipe));
    PullingAsyncPipelineExecutor executor(pipeline);

    if (need_render_progress)
    {
        pipeline.setProgressCallback([this](const Progress & progress){ onProgress(progress); });
    }

    Block block;
    while (executor.pull(block))
    {
        if (!cancelled && QueryInterruptHandler::cancelled())
        {
            cancelQuery();
            executor.cancel();
            return;
        }

        /// Check if server send Log packet
        receiveLogsAndProfileEvents(parsed_query);

        /// Check if server send Exception packet
        auto packet_type = connection->checkPacket(0);
        if (packet_type && *packet_type == Protocol::Server::Exception)
        {
            /**
             * We're exiting with error, so it makes sense to kill the
             * input stream without waiting for it to complete.
             */
            executor.cancel();
            return;
        }

        if (block)
        {
            connection->sendData(block, /* name */"", /* scalar */false);
            processed_rows += block.rows();
        }
    }

    if (!have_more_data)
        connection->sendData({}, "", false);
}
catch (...)
{
    connection->sendCancel();
    receiveEndOfQuery();
    throw;
}

void ClientBase::sendDataFromStdin(Block & sample, const ColumnsDescription & columns_description, ASTPtr parsed_query)
{
    /// Send data read from stdin.
    try
    {
        sendDataFrom(std_in, sample, columns_description, parsed_query);
    }
    catch (Exception & e)
    {
        e.addMessage("data for INSERT was parsed from stdin");
        throw;
    }
}


/// Process Log packets, used when inserting data by blocks
void ClientBase::receiveLogsAndProfileEvents(ASTPtr parsed_query)
{
    auto packet_type = connection->checkPacket(0);

    while (packet_type && (*packet_type == Protocol::Server::Log
            || *packet_type == Protocol::Server::ProfileEvents
            || *packet_type == Protocol::Server::TimezoneUpdate))
    {
        receiveAndProcessPacket(parsed_query, false);
        packet_type = connection->checkPacket(0);
    }
}


/// Process Log packets, exit when receive Exception or EndOfStream
bool ClientBase::receiveEndOfQuery()
{
    while (true)
    {
        Packet packet = connection->receivePacket();

        switch (packet.type)
        {
            case Protocol::Server::EndOfStream:
                onEndOfStream();
                return true;

            case Protocol::Server::Exception:
                onReceiveExceptionFromServer(std::move(packet.exception));
                return false;

            case Protocol::Server::Log:
                onLogData(packet.block);
                break;

            case Protocol::Server::Progress:
                onProgress(packet.progress);
                break;

            case Protocol::Server::ProfileEvents:
                onProfileEvents(packet.block);
                break;

            case Protocol::Server::TimezoneUpdate:
                onTimezoneUpdate(packet.server_timezone);
                break;

            default:
                throw NetException(ErrorCodes::UNEXPECTED_PACKET_FROM_SERVER,
                    "Unexpected packet from server (expected Exception, EndOfStream, Log, Progress or ProfileEvents. Got {})",
                    String(Protocol::Server::toString(packet.type)));
        }
    }
}

void ClientBase::cancelQuery()
{
    connection->sendCancel();
    if (need_render_progress && tty_buf)
        progress_indication.clearProgressOutput(*tty_buf);

    if (is_interactive)
        std::cout << "Cancelling query." << std::endl;

    cancelled = true;
}

void ClientBase::processParsedSingleQuery(const String & full_query, const String & query_to_execute,
        ASTPtr parsed_query, std::optional<bool> echo_query_, bool report_error)
{
    resetOutput();
    have_error = false;
    cancelled = false;
    cancelled_printed = false;
    client_exception.reset();
    server_exception.reset();

    if (echo_query_ && *echo_query_)
    {
        writeString(full_query, std_out);
        writeChar('\n', std_out);
        std_out.next();
    }

    if (is_interactive)
    {
        global_context->setCurrentQueryId("");
        // Generate a new query_id
        for (const auto & query_id_format : query_id_formats)
        {
            writeString(query_id_format.first, std_out);
            writeString(fmt::format(fmt::runtime(query_id_format.second), fmt::arg("query_id", global_context->getCurrentQueryId())), std_out);
            writeChar('\n', std_out);
            std_out.next();
        }
    }

    if (const auto * set_query = parsed_query->as<ASTSetQuery>())
    {
        const auto * logs_level_field = set_query->changes.tryGet(std::string_view{"send_logs_level"});
        if (logs_level_field)
        {
            auto logs_level = logs_level_field->safeGet<String>();
            /// Check that setting value is correct before updating logger level.
            SettingFieldLogsLevelTraits::fromString(logs_level);
            updateLoggerLevel(logs_level);
        }
    }

    if (const auto * create_user_query = parsed_query->as<ASTCreateUserQuery>())
    {
        if (!create_user_query->attach && create_user_query->auth_data)
        {
            if (const auto * auth_data = create_user_query->auth_data->as<ASTAuthenticationData>())
            {
                auto password = auth_data->getPassword();

                if (password)
                    global_context->getAccessControl().checkPasswordComplexityRules(*password);
            }
        }
    }

    processed_rows = 0;
    written_first_block = false;
    progress_indication.resetProgress();
    profile_events.watch.restart();

    {
        /// Temporarily apply query settings to context.
        std::optional<Settings> old_settings;
        SCOPE_EXIT_SAFE({
            if (old_settings)
                global_context->setSettings(*old_settings);
        });

        auto apply_query_settings = [&](const IAST & settings_ast)
        {
            if (!old_settings)
                old_settings.emplace(global_context->getSettingsRef());
            global_context->applySettingsChanges(settings_ast.as<ASTSetQuery>()->changes);
            global_context->resetSettingsToDefaultValue(settings_ast.as<ASTSetQuery>()->default_settings);
        };

        const auto * insert = parsed_query->as<ASTInsertQuery>();
        if (const auto * select = parsed_query->as<ASTSelectQuery>(); select && select->settings())
            apply_query_settings(*select->settings());
        else if (const auto * select_with_union = parsed_query->as<ASTSelectWithUnionQuery>())
        {
            const ASTs & children = select_with_union->list_of_selects->children;
            if (!children.empty())
            {
                // On the client it is enough to apply settings only for the
                // last SELECT, since the only thing that is important to apply
                // on the client is format settings.
                const auto * last_select = children.back()->as<ASTSelectQuery>();
                if (last_select && last_select->settings())
                {
                    apply_query_settings(*last_select->settings());
                }
            }
        }
        else if (const auto * query_with_output = parsed_query->as<ASTQueryWithOutput>(); query_with_output && query_with_output->settings_ast)
            apply_query_settings(*query_with_output->settings_ast);
        else if (insert && insert->settings_ast)
            apply_query_settings(*insert->settings_ast);

        if (!connection->checkConnected(connection_parameters.timeouts))
            connect();

        ASTPtr input_function;
        if (insert && insert->select)
            insert->tryFindInputFunction(input_function);

        bool is_async_insert_with_inlined_data = global_context->getSettingsRef().async_insert && insert && insert->hasInlinedData();

        if (is_async_insert_with_inlined_data)
        {
            bool have_data_in_stdin = !is_interactive && !stdin_is_a_tty && isStdinNotEmptyAndValid(std_in);
            bool have_external_data = have_data_in_stdin || insert->infile;

            if (have_external_data)
                throw Exception(ErrorCodes::NOT_IMPLEMENTED,
                    "Processing async inserts with both inlined and external data (from stdin or infile) is not supported");
        }

        /// INSERT query for which data transfer is needed (not an INSERT SELECT or input()) is processed separately.
        if (insert && (!insert->select || input_function) && !is_async_insert_with_inlined_data)
        {
            if (input_function && insert->format.empty())
                throw Exception(ErrorCodes::INVALID_USAGE_OF_INPUT, "FORMAT must be specified for function input()");

            processInsertQuery(query_to_execute, parsed_query);
        }
        else
            processOrdinaryQuery(query_to_execute, parsed_query);
    }

    /// Do not change context (current DB, settings) in case of an exception.
    if (!have_error)
    {
        if (const auto * set_query = parsed_query->as<ASTSetQuery>())
        {
            /// Save all changes in settings to avoid losing them if the connection is lost.
            for (const auto & change : set_query->changes)
            {
                if (change.name == "profile")
                    current_profile = change.value.safeGet<String>();
                else
                    global_context->applySettingChange(change);
            }
            global_context->resetSettingsToDefaultValue(set_query->default_settings);

            /// Query parameters inside SET queries should be also saved on the client side
            ///  to override their previous definitions set with --param_* arguments
            ///  and for substitutions to work inside INSERT ... VALUES queries
            for (const auto & [name, value] : set_query->query_parameters)
                query_parameters.insert_or_assign(name, value);

            global_context->addQueryParameters(NameToNameMap{set_query->query_parameters.begin(), set_query->query_parameters.end()});
        }
        if (const auto * use_query = parsed_query->as<ASTUseQuery>())
        {
            const String & new_database = use_query->getDatabase();
            /// If the client initiates the reconnection, it takes the settings from the config.
            config().setString("database", new_database);
            /// If the connection initiates the reconnection, it uses its variable.
            connection->setDefaultDatabase(new_database);
        }
    }

    /// Always print last block (if it was not printed already)
    if (profile_events.last_block)
    {
        initLogsOutputStream();
        if (need_render_progress && tty_buf)
            progress_indication.clearProgressOutput(*tty_buf);
        logs_out_stream->writeProfileEvents(profile_events.last_block);
        logs_out_stream->flush();

        profile_events.last_block = {};
    }

    if (is_interactive)
    {
        std::cout << std::endl;
        if (!server_exception || processed_rows != 0)
            std::cout << processed_rows << " row" << (processed_rows == 1 ? "" : "s") << " in set. ";
        std::cout << "Elapsed: " << progress_indication.elapsedSeconds() << " sec. ";
        progress_indication.writeFinalProgress();
        std::cout << std::endl << std::endl;
    }
    else if (print_time_to_stderr)
    {
        std::cerr << progress_indication.elapsedSeconds() << "\n";
    }

    if (!is_interactive && print_num_processed_rows)
    {
        std::cout << "Processed rows: " << processed_rows << "\n";
    }

    if (have_error && report_error)
        processError(full_query);
}


MultiQueryProcessingStage ClientBase::analyzeMultiQueryText(
    const char *& this_query_begin, const char *& this_query_end, const char * all_queries_end,
    String & query_to_execute, ASTPtr & parsed_query, const String & all_queries_text,
    std::unique_ptr<Exception> & current_exception)
{
    if (!is_interactive && cancelled)
        return MultiQueryProcessingStage::QUERIES_END;

    if (this_query_begin >= all_queries_end)
        return MultiQueryProcessingStage::QUERIES_END;

    // Remove leading empty newlines and other whitespace, because they
    // are annoying to filter in the query log. This is mostly relevant for
    // the tests.
    while (this_query_begin < all_queries_end && isWhitespaceASCII(*this_query_begin))
        ++this_query_begin;

    if (this_query_begin >= all_queries_end)
        return MultiQueryProcessingStage::QUERIES_END;

    unsigned max_parser_depth = static_cast<unsigned>(global_context->getSettingsRef().max_parser_depth);
    unsigned max_parser_backtracks = static_cast<unsigned>(global_context->getSettingsRef().max_parser_backtracks);

    // If there are only comments left until the end of file, we just
    // stop. The parser can't handle this situation because it always
    // expects that there is some query that it can parse.
    // We can get into this situation because the parser also doesn't
    // skip the trailing comments after parsing a query. This is because
    // they may as well be the leading comments for the next query,
    // and it makes more sense to treat them as such.
    {
        Tokens tokens(this_query_begin, all_queries_end);
        IParser::Pos token_iterator(tokens, max_parser_depth, max_parser_backtracks);
        if (!token_iterator.isValid())
            return MultiQueryProcessingStage::QUERIES_END;
    }

    this_query_end = this_query_begin;
    try
    {
        parsed_query = parseQuery(this_query_end, all_queries_end,
            global_context->getSettingsRef(),
            /*allow_multi_statements=*/ true,
            is_interactive,
            ignore_error);
    }
    catch (const Exception & e)
    {
        current_exception.reset(e.clone());
        return MultiQueryProcessingStage::PARSING_EXCEPTION;
    }

    if (!parsed_query)
    {
        if (ignore_error)
        {
            Tokens tokens(this_query_begin, all_queries_end);
            IParser::Pos token_iterator(tokens, max_parser_depth, max_parser_backtracks);
            while (token_iterator->type != TokenType::Semicolon && token_iterator.isValid())
                ++token_iterator;
            this_query_begin = token_iterator->end;

            return MultiQueryProcessingStage::CONTINUE_PARSING;
        }

        return MultiQueryProcessingStage::PARSING_FAILED;
    }

    // INSERT queries may have the inserted data in the query text
    // that follow the query itself, e.g. "insert into t format CSV 1;2".
    // They need special handling. First of all, here we find where the
    // inserted data ends. In multi-query mode, it is delimited by a
    // newline.
    // The VALUES format needs even more handling - we also allow the
    // data to be delimited by semicolon. This case is handled later by
    // the format parser itself.
    // We can't do multiline INSERTs with inline data, because most
    // row input formats (e.g. TSV) can't tell when the input stops,
    // unlike VALUES.
    auto * insert_ast = parsed_query->as<ASTInsertQuery>();
    const char * query_to_execute_end = this_query_end;

    if (insert_ast && insert_ast->data)
    {
        this_query_end = find_first_symbols<'\n'>(insert_ast->data, all_queries_end);
        insert_ast->end = this_query_end;
        query_to_execute_end = isSyncInsertWithData(*insert_ast, global_context) ? insert_ast->data : this_query_end;
    }

    query_to_execute = all_queries_text.substr(this_query_begin - all_queries_text.data(), query_to_execute_end - this_query_begin);

    // Try to include the trailing comment with test hints. It is just
    // a guess for now, because we don't yet know where the query ends
    // if it is an INSERT query with inline data. We will do it again
    // after we have processed the query. But even this guess is
    // beneficial so that we see proper trailing comments in "echo" and
    // server log.
    adjustQueryEnd(this_query_end, all_queries_end, max_parser_depth, max_parser_backtracks);
    return MultiQueryProcessingStage::EXECUTE_QUERY;
}


bool ClientBase::executeMultiQuery(const String & all_queries_text)
{
    bool echo_query = echo_queries;

    {
        /// disable logs if expects errors
        TestHint test_hint(all_queries_text);
        if (test_hint.hasClientErrors() || test_hint.hasServerErrors())
            processTextAsSingleQuery("SET send_logs_level = 'fatal'");
    }

    /// Test tags are started with "--" so they are interpreted as comments anyway.
    /// But if the echo is enabled we have to remove the test tags from `all_queries_text`
    /// because we don't want test tags to be echoed.
    size_t test_tags_length = getTestTagsLength(all_queries_text);

    /// Several queries separated by ';'.
    /// INSERT data is ended by the end of line, not ';'.
    /// An exception is VALUES format where we also support semicolon in
    /// addition to end of line.
    const char * this_query_begin = all_queries_text.data() + test_tags_length;
    const char * this_query_end;
    const char * all_queries_end = all_queries_text.data() + all_queries_text.size();

    String full_query; // full_query is the query + inline INSERT data + trailing comments (the latter is our best guess for now).
    String query_to_execute;
    ASTPtr parsed_query;
    std::unique_ptr<Exception> current_exception;

    while (true)
    {
        auto stage = analyzeMultiQueryText(this_query_begin, this_query_end, all_queries_end,
                                           query_to_execute, parsed_query, all_queries_text, current_exception);
        switch (stage)
        {
            case MultiQueryProcessingStage::QUERIES_END:
            case MultiQueryProcessingStage::PARSING_FAILED:
            {
                return true;
            }
            case MultiQueryProcessingStage::CONTINUE_PARSING:
            {
                continue;
            }
            case MultiQueryProcessingStage::PARSING_EXCEPTION:
            {
                this_query_end = find_first_symbols<'\n'>(this_query_end, all_queries_end);

                // Try to find test hint for syntax error. We don't know where
                // the query ends because we failed to parse it, so we consume
                // the entire line.
                TestHint hint(String(this_query_begin, this_query_end - this_query_begin));
                if (hint.hasServerErrors())
                {
                    // Syntax errors are considered as client errors
                    current_exception->addMessage("\nExpected server error: {}.", hint.serverErrors());
                    current_exception->rethrow();
                }

                if (!hint.hasExpectedClientError(current_exception->code()))
                {
                    if (hint.hasClientErrors())
                        current_exception->addMessage("\nExpected client error: {}.", hint.clientErrors());

                    current_exception->rethrow();
                }

                /// It's expected syntax error, skip the line
                this_query_begin = this_query_end;
                current_exception.reset();

                continue;
            }
            case MultiQueryProcessingStage::EXECUTE_QUERY:
            {
                full_query = all_queries_text.substr(this_query_begin - all_queries_text.data(), this_query_end - this_query_begin);
                if (query_fuzzer_runs)
                {
                    if (!processWithFuzzing(full_query))
                        return false;

                    this_query_begin = this_query_end;
                    continue;
                }

                // Now we know for sure where the query ends.
                // Look for the hint in the text of query + insert data + trailing
                // comments, e.g. insert into t format CSV 'a' -- { serverError 123 }.
                // Use the updated query boundaries we just calculated.
                TestHint test_hint(full_query);

                // Echo all queries if asked; makes for a more readable reference file.
                echo_query = test_hint.echoQueries().value_or(echo_query);

                try
                {
                    processParsedSingleQuery(full_query, query_to_execute, parsed_query, echo_query, false);
                }
                catch (...)
                {
                    // Surprisingly, this is a client error. A server error would
                    // have been reported without throwing (see onReceiveSeverException()).
                    client_exception = std::make_unique<Exception>(getCurrentExceptionMessageAndPattern(print_stack_trace), getCurrentExceptionCode());
                    have_error = true;
                }

                // Check whether the error (or its absence) matches the test hints
                // (or their absence).
                bool error_matches_hint = true;
                if (have_error)
                {
                    if (test_hint.hasServerErrors())
                    {
                        if (!server_exception)
                        {
                            error_matches_hint = false;
                            fmt::print(stderr, "Expected server error code '{}' but got no server error (query: {}).\n",
                                       test_hint.serverErrors(), full_query);
                        }
                        else if (!test_hint.hasExpectedServerError(server_exception->code()))
                        {
                            error_matches_hint = false;
                            fmt::print(stderr, "Expected server error code: {} but got: {} (query: {}).\n",
                                       test_hint.serverErrors(), server_exception->code(), full_query);
                        }
                    }
                    if (test_hint.hasClientErrors())
                    {
                        if (!client_exception)
                        {
                            error_matches_hint = false;
                            fmt::print(stderr, "Expected client error code '{}' but got no client error (query: {}).\n",
                                       test_hint.clientErrors(), full_query);
                        }
                        else if (!test_hint.hasExpectedClientError(client_exception->code()))
                        {
                            error_matches_hint = false;
                            fmt::print(stderr, "Expected client error code '{}' but got '{}' (query: {}).\n",
                                       test_hint.clientErrors(), client_exception->code(), full_query);
                        }
                    }
                    if (!test_hint.hasClientErrors() && !test_hint.hasServerErrors())
                    {
                        // No error was expected but it still occurred. This is the
                        // default case without test hint, doesn't need additional
                        // diagnostics.
                        error_matches_hint = false;
                    }
                }
                else
                {
                    if (test_hint.hasClientErrors())
                    {
                        error_matches_hint = false;
                        fmt::print(stderr,
                                   "The query succeeded but the client error '{}' was expected (query: {}).\n",
                                   test_hint.clientErrors(), full_query);
                    }
                    if (test_hint.hasServerErrors())
                    {
                        error_matches_hint = false;
                        fmt::print(stderr,
                                   "The query succeeded but the server error '{}' was expected (query: {}).\n",
                                   test_hint.serverErrors(), full_query);
                    }
                }

                // If the error is expected, force reconnect and ignore it.
                if (have_error && error_matches_hint)
                {
                    client_exception.reset();
                    server_exception.reset();

                    have_error = false;

                    if (!connection->checkConnected(connection_parameters.timeouts))
                        connect();
                }

                // For INSERTs with inline data: use the end of inline data as
                // reported by the format parser (it is saved in sendData()).
                // This allows us to handle queries like:
                //   insert into t values (1); select 1
                // , where the inline data is delimited by semicolon and not by a
                // newline.
                auto * insert_ast = parsed_query->as<ASTInsertQuery>();
                if (insert_ast && isSyncInsertWithData(*insert_ast, global_context))
                {
                    this_query_end = insert_ast->end;
                    adjustQueryEnd(
                        this_query_end, all_queries_end,
                        static_cast<unsigned>(global_context->getSettingsRef().max_parser_depth),
                        static_cast<unsigned>(global_context->getSettingsRef().max_parser_backtracks));
                }

                // Report error.
                if (have_error)
                    processError(full_query);

                // Stop processing queries if needed.
                if (have_error && !ignore_error)
                    return is_interactive;

                this_query_begin = this_query_end;
                break;
            }
        }
    }
}


bool ClientBase::processQueryText(const String & text)
{
    auto trimmed_input = trim(text, [](char c) { return isWhitespaceASCII(c) || c == ';'; });

    if (exit_strings.end() != exit_strings.find(trimmed_input))
        return false;

    if (trimmed_input.starts_with("\\i"))
    {
        size_t skip_prefix_size = std::strlen("\\i");
        auto file_name = trim(
            trimmed_input.substr(skip_prefix_size, trimmed_input.size() - skip_prefix_size),
            [](char c) { return isWhitespaceASCII(c); });

        return processMultiQueryFromFile(file_name);
    }

    if (!is_multiquery)
    {
        assert(!query_fuzzer_runs);
        processTextAsSingleQuery(text);

        return true;
    }

    if (query_fuzzer_runs)
    {
        processWithFuzzing(text);
        return true;
    }

    return executeMultiQuery(text);
}


String ClientBase::prompt() const
{
    return prompt_by_server_display_name;
}


void ClientBase::initQueryIdFormats()
{
    if (!query_id_formats.empty())
        return;

    /// Initialize query_id_formats if any
    if (config().has("query_id_formats"))
    {
        Poco::Util::AbstractConfiguration::Keys keys;
        config().keys("query_id_formats", keys);
        for (const auto & name : keys)
            query_id_formats.emplace_back(name + ":", config().getString("query_id_formats." + name));
    }

    if (query_id_formats.empty())
        query_id_formats.emplace_back("Query id:", " {query_id}\n");
}


bool ClientBase::addMergeTreeSettings(ASTCreateQuery & ast_create)
{
    if (ast_create.attach
        || !ast_create.storage
        || !ast_create.storage->isExtendedStorageDefinition()
        || !ast_create.storage->engine
        || ast_create.storage->engine->name.find("MergeTree") == std::string::npos)
        return false;

    auto all_changed = cmd_merge_tree_settings.allChanged();
    if (all_changed.begin() == all_changed.end())
        return false;

    if (!ast_create.storage->settings)
    {
        auto settings_ast = std::make_shared<ASTSetQuery>();
        settings_ast->is_standalone = false;
        ast_create.storage->set(ast_create.storage->settings, settings_ast);
    }

    auto & storage_settings = *ast_create.storage->settings;
    bool added_new_setting = false;

    for (const auto & setting : all_changed)
    {
        if (!storage_settings.changes.tryGet(setting.getName()))
        {
            storage_settings.changes.emplace_back(setting.getName(), setting.getValue());
            added_new_setting = true;
        }
    }

    return added_new_setting;
}

void ClientBase::runInteractive()
{
    if (config().has("query_id"))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "query_id could be specified only in non-interactive mode");
    if (print_time_to_stderr)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "time option could be specified only in non-interactive mode");

    initQueryIdFormats();

    /// Initialize DateLUT here to avoid counting time spent here as query execution time.
    const auto local_tz = DateLUT::instance().getTimeZone();

    suggest.emplace();
    if (load_suggestions)
    {
        /// Load suggestion data from the server.
        if (global_context->getApplicationType() == Context::ApplicationType::CLIENT)
            suggest->load<Connection>(global_context, connection_parameters, config().getInt("suggestion_limit"), wait_for_suggestions_to_load);
        else if (global_context->getApplicationType() == Context::ApplicationType::LOCAL)
            suggest->load<LocalConnection>(global_context, connection_parameters, config().getInt("suggestion_limit"), wait_for_suggestions_to_load);
    }

    if (home_path.empty())
    {
        const char * home_path_cstr = getenv("HOME"); // NOLINT(concurrency-mt-unsafe)
        if (home_path_cstr)
            home_path = home_path_cstr;
    }

    /// Load command history if present.
    if (config().has("history_file"))
        history_file = config().getString("history_file");
    else
    {
        auto * history_file_from_env = getenv("CLICKHOUSE_HISTORY_FILE"); // NOLINT(concurrency-mt-unsafe)
        if (history_file_from_env)
            history_file = history_file_from_env;
        else if (!home_path.empty())
            history_file = home_path + "/.clickhouse-client-history";
    }

    if (!history_file.empty() && !fs::exists(history_file))
    {
        /// Avoid TOCTOU issue.
        try
        {
            FS::createFile(history_file);
        }
        catch (const ErrnoException & e)
        {
            if (e.getErrno() != EEXIST)
            {
                std::cerr << getCurrentExceptionMessage(false) << '\n';
            }
        }
    }

    LineReader::Patterns query_extenders = {"\\"};
    LineReader::Patterns query_delimiters = {";", "\\G", "\\G;"};
    char word_break_characters[] = " \t\v\f\a\b\r\n`~!@#$%^&*()-=+[{]}\\|;:'\",<.>/?";

#if USE_REPLXX
    replxx::Replxx::highlighter_callback_t highlight_callback{};
    if (config().getBool("highlight", true))
        highlight_callback = highlight;

    ReplxxLineReader lr(
        *suggest,
        history_file,
        config().has("multiline"),
        query_extenders,
        query_delimiters,
        word_break_characters,
        highlight_callback);
#else
    LineReader lr(
        history_file,
        config().has("multiline"),
        query_extenders,
        query_delimiters,
        word_break_characters);
#endif

    static const std::initializer_list<std::pair<String, String>> backslash_aliases =
        {
            { "\\l", "SHOW DATABASES" },
            { "\\d", "SHOW TABLES" },
            { "\\c", "USE" },
        };

    static const std::initializer_list<String> repeat_last_input_aliases =
        {
            ".",  /// Vim shortcut
            "/"   /// Oracle SQL Plus shortcut
        };

    String last_input;

    do
    {
        String input;
        {
            /// Enable bracketed-paste-mode so that we are able to paste multiline queries as a whole.
            /// But keep it disabled outside of query input, because it breaks password input
            /// (e.g. if we need to reconnect and show a password prompt).
            /// (Alternatively, we could make the password input ignore the control sequences.)
            lr.enableBracketedPaste();
            SCOPE_EXIT({ lr.disableBracketedPaste(); });

            input = lr.readLine(prompt(), ":-] ");
        }

        if (input.empty())
            break;

        has_vertical_output_suffix = false;
        if (input.ends_with("\\G") || input.ends_with("\\G;"))
        {
            if (input.ends_with("\\G"))
                input.resize(input.size() - 2);
            else if (input.ends_with("\\G;"))
                input.resize(input.size() - 3);

            has_vertical_output_suffix = true;
        }

        for (const auto & [alias, command] : backslash_aliases)
        {
            auto it = std::search(input.begin(), input.end(), alias.begin(), alias.end());
            if (it != input.end() && std::all_of(input.begin(), it, isWhitespaceASCII))
            {
                it += alias.size();
                if (it == input.end() || isWhitespaceASCII(*it))
                {
                    String new_input = command;
                    // append the rest of input to the command
                    // for parameters support, e.g. \c db_name -> USE db_name
                    new_input.append(it, input.end());
                    input = std::move(new_input);
                    break;
                }
            }
        }

        for (const auto & alias : repeat_last_input_aliases)
        {
            if (input == alias)
            {
                input  = last_input;
                break;
            }
        }

        if (suggest && suggest->getLastError() == ErrorCodes::USER_SESSION_LIMIT_EXCEEDED)
        {
            // If a separate connection loading suggestions failed to open a new session,
            // use the main session to receive them.
            suggest->load(*connection, connection_parameters.timeouts, config().getInt("suggestion_limit"), global_context->getClientInfo());
        }

        try
        {
            if (!processQueryText(input))
                break;
            last_input = input;
        }
        catch (const Exception & e)
        {
            /// We don't need to handle the test hints in the interactive mode.
            std::cerr << "Exception on client:" << std::endl << getExceptionMessage(e, print_stack_trace, true) << std::endl << std::endl;
            client_exception.reset(e.clone());
        }

        if (client_exception)
        {
            /// client_exception may have been set above or elsewhere.
            /// Client-side exception during query execution can result in the loss of
            /// sync in the connection protocol.
            /// So we reconnect and allow to enter the next query.
            if (!connection->checkConnected(connection_parameters.timeouts))
                connect();
        }
    }
    while (true);

    if (isNewYearMode())
        std::cout << "Happy new year." << std::endl;
    else if (isChineseNewYearMode(local_tz))
        std::cout << "Happy Chinese new year. 春节快乐!" << std::endl;
    else
        std::cout << "Bye." << std::endl;
}


bool ClientBase::processMultiQueryFromFile(const String & file_name)
{
    String queries_from_file;

    ReadBufferFromFile in(file_name);
    readStringUntilEOF(queries_from_file, in);

    if (!has_log_comment)
    {
        Settings settings = global_context->getSettings();
        /// NOTE: cannot use even weakly_canonical() since it fails for /dev/stdin due to resolving of "pipe:[X]"
        settings.log_comment = fs::absolute(fs::path(file_name));
        global_context->setSettings(settings);
    }

    return executeMultiQuery(queries_from_file);
}


void ClientBase::runNonInteractive()
{
    if (delayed_interactive)
        initQueryIdFormats();

    if (!queries_files.empty())
    {
        for (const auto & queries_file : queries_files)
        {
            for (const auto & interleave_file : interleave_queries_files)
                if (!processMultiQueryFromFile(interleave_file))
                    return;

            if (!processMultiQueryFromFile(queries_file))
                return;
        }

        return;
    }

    if (!queries.empty())
    {
        for (const auto & query : queries)
        {
            if (query_fuzzer_runs)
            {
                if (!processWithFuzzing(query))
                    return;
            }
            else
            {
                if (!processQueryText(query))
                    return;
            }
        }
    }
    else
    {
        /// If 'query' parameter is not set, read a query from stdin.
        /// The query is read entirely into memory (streaming is disabled).
        ReadBufferFromFileDescriptor in(STDIN_FILENO);
        String text;
        readStringUntilEOF(text, in);
        if (query_fuzzer_runs)
            processWithFuzzing(text);
        else
            processQueryText(text);
    }
}


#if defined(FUZZING_MODE)
extern "C" int LLVMFuzzerRunDriver(int * argc, char *** argv, int (*callback)(const uint8_t * data, size_t size));
ClientBase * app;

void ClientBase::runLibFuzzer()
{
    app = this;
    std::vector<String> fuzzer_args_holder;

    if (const char * fuzzer_args_env = getenv("FUZZER_ARGS")) // NOLINT(concurrency-mt-unsafe)
        boost::split(fuzzer_args_holder, fuzzer_args_env, isWhitespaceASCII, boost::token_compress_on);

    std::vector<char *> fuzzer_args;
    fuzzer_args.push_back(argv0);
    for (auto & arg : fuzzer_args_holder)
        fuzzer_args.emplace_back(arg.data());

    int fuzzer_argc = fuzzer_args.size();
    char ** fuzzer_argv = fuzzer_args.data();

    LLVMFuzzerRunDriver(&fuzzer_argc, &fuzzer_argv, [](const uint8_t * data, size_t size)
    {
        try
        {
            String query(reinterpret_cast<const char *>(data), size);
            app->processQueryText(query);
        }
        catch (...)
        {
            return -1;
        }

        return 0;
    });
}
#else
void ClientBase::runLibFuzzer() {}
#endif


void ClientBase::clearTerminal()
{
    /// Clear from cursor until end of screen.
    /// It is needed if garbage is left in terminal.
    /// Show cursor. It can be left hidden by invocation of previous programs.
    /// A test for this feature: perl -e 'print "x"x100000'; echo -ne '\033[0;0H\033[?25l'; clickhouse-client
    std::cout << "\033[0J" "\033[?25h";
}


void ClientBase::showClientVersion()
{
    std::cout << VERSION_NAME << " " + getName() + " version " << VERSION_STRING << VERSION_OFFICIAL << "." << std::endl;
}

namespace
{

/// Define transparent hash to we can use
/// std::string_view with the containers
struct TransparentStringHash
{
    using is_transparent = void;
    size_t operator()(std::string_view txt) const
    {
        return std::hash<std::string_view>{}(txt);
    }
};

/*
 * This functor is used to parse command line arguments and replace dashes with underscores,
 * allowing options to be specified using either dashes or underscores.
 */
class OptionsAliasParser
{
public:
    explicit OptionsAliasParser(const boost::program_options::options_description& options)
    {
        options_names.reserve(options.options().size());
        for (const auto& option : options.options())
            options_names.insert(option->long_name());
    }

    /*
     * Parses arguments by replacing dashes with underscores, and matches the resulting name with known options
     * Implements boost::program_options::ext_parser logic
     */
    std::pair<std::string, std::string> operator()(const std::string & token) const
    {
        if (!token.starts_with("--"))
            return {};
        std::string arg = token.substr(2);

        // divide token by '=' to separate key and value if options style=long_allow_adjacent
        auto pos_eq = arg.find('=');
        std::string key = arg.substr(0, pos_eq);

        if (options_names.contains(key))
            // option does not require any changes, because it is already correct
            return {};

        std::replace(key.begin(), key.end(), '-', '_');
        if (!options_names.contains(key))
            // after replacing '-' with '_' argument is still unknown
            return {};

        std::string value;
        if (pos_eq != std::string::npos && pos_eq < arg.size())
            value = arg.substr(pos_eq + 1);

        return {key, value};
    }

private:
    std::unordered_set<std::string> options_names;
};

}


void ClientBase::parseAndCheckOptions(OptionsDescription & options_description, po::variables_map & options, Arguments & arguments)
{
    if (allow_repeated_settings)
        addProgramOptionsAsMultitokens(cmd_settings, options_description.main_description.value());
    else
        addProgramOptions(cmd_settings, options_description.main_description.value());

    if (allow_merge_tree_settings)
    {
        /// Add merge tree settings manually, because names of some settings
        /// may clash. Query settings have higher priority and we just
        /// skip ambiguous merge tree settings.
        auto & main_options = options_description.main_description.value();

        std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> main_option_names;
        for (const auto & option : main_options.options())
            main_option_names.insert(option->long_name());

        for (const auto & setting : cmd_merge_tree_settings.all())
        {
            const auto add_setting = [&](const std::string_view name)
            {
                if (auto it = main_option_names.find(name); it != main_option_names.end())
                    return;

                if (allow_repeated_settings)
                    addProgramOptionAsMultitoken(cmd_merge_tree_settings, main_options, name, setting);
                else
                    addProgramOption(cmd_merge_tree_settings, main_options, name, setting);
            };

            const auto & setting_name = setting.getName();

            add_setting(setting_name);

            const auto & settings_to_aliases = MergeTreeSettings::Traits::settingsToAliases();
            if (auto it = settings_to_aliases.find(setting_name); it != settings_to_aliases.end())
            {
                for (const auto alias : it->second)
                {
                    add_setting(alias);
                }
            }
        }
    }

    /// Parse main commandline options.
    auto parser = po::command_line_parser(arguments)
                      .options(options_description.main_description.value())
                      .extra_parser(OptionsAliasParser(options_description.main_description.value()))
                      .allow_unregistered();
    po::parsed_options parsed = parser.run();

    /// Check unrecognized options without positional options.
    auto unrecognized_options = po::collect_unrecognized(parsed.options, po::collect_unrecognized_mode::exclude_positional);
    if (!unrecognized_options.empty())
    {
        auto hints = this->getHints(unrecognized_options[0]);
        if (!hints.empty())
            throw Exception(ErrorCodes::UNRECOGNIZED_ARGUMENTS, "Unrecognized option '{}'. Maybe you meant {}",
                            unrecognized_options[0], toString(hints));

        throw Exception(ErrorCodes::UNRECOGNIZED_ARGUMENTS, "Unrecognized option '{}'", unrecognized_options[0]);
    }

    /// Check positional options.
    for (const auto & op : parsed.options)
    {
        if (!op.unregistered && op.string_key.empty() && !op.original_tokens[0].starts_with("--")
            && !op.original_tokens[0].empty() && !op.value.empty())
        {
            /// Two special cases for better usability:
            /// - if the option contains a whitespace, it might be a query: clickhouse "SELECT 1"
            /// These are relevant for interactive usage - user-friendly, but questionable in general.
            /// In case of ambiguity or for scripts, prefer using proper options.

            const auto & token = op.original_tokens[0];
            po::variable_value value(boost::any(op.value), false);

            const char * option;
            if (token.contains(' '))
                option = "query";
            else
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Positional option `{}` is not supported.", token);

            if (!options.emplace(option, value).second)
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Positional option `{}` is not supported.", token);
        }
    }

    po::store(parsed, options);
}


void ClientBase::init(int argc, char ** argv)
{
    namespace po = boost::program_options;

    /// Don't parse options with Poco library, we prefer neat boost::program_options.
    stopOptionsProcessing();

    stdin_is_a_tty = isatty(STDIN_FILENO);
    stdout_is_a_tty = isatty(STDOUT_FILENO);
    stderr_is_a_tty = isatty(STDERR_FILENO);
    terminal_width = getTerminalWidth();

    std::vector<Arguments> external_tables_arguments;
    Arguments common_arguments = {""}; /// 0th argument is ignored.
    std::vector<Arguments> hosts_and_ports_arguments;

    if (argc)
        argv0 = argv[0];
    readArguments(argc, argv, common_arguments, external_tables_arguments, hosts_and_ports_arguments);

    /// Support for Unicode dashes
    /// Interpret Unicode dashes as default double-hyphen
    for (auto & arg : common_arguments)
    {
        // replace em-dash(U+2014)
        boost::replace_all(arg, "—", "--");
        // replace en-dash(U+2013)
        boost::replace_all(arg, "–", "--");
        // replace mathematical minus(U+2212)
        boost::replace_all(arg, "−", "--");
    }


    OptionsDescription options_description;
    options_description.main_description.emplace(createOptionsDescription("Main options", terminal_width));

    /// Common options for clickhouse-client and clickhouse-local.
    options_description.main_description->add_options()
        ("help", "print usage summary, combine with --verbose to display all options")
        ("verbose", "print query and other debugging info")
        ("version,V", "print version information and exit")
        ("version-clean", "print version in machine-readable format and exit")

        ("config-file,C", po::value<std::string>(), "config-file path")

        ("query,q", po::value<std::vector<std::string>>()->multitoken(), R"(query; can be specified multiple times (--query "SELECT 1" --query "SELECT 2"...))")
        ("queries-file", po::value<std::vector<std::string>>()->multitoken(), "file path with queries to execute; multiple files can be specified (--queries-file file1 file2...)")
        ("multiquery,n", "If specified, multiple queries separated by semicolons can be listed after --query. For convenience, it is also possible to omit --query and pass the queries directly after --multiquery.")
        ("multiline,m", "If specified, allow multiline queries (do not send the query on Enter)")
        ("database,d", po::value<std::string>(), "database")
        ("query_kind", po::value<std::string>()->default_value("initial_query"), "One of initial_query/secondary_query/no_query")
        ("query_id", po::value<std::string>(), "query_id")

        ("history_file", po::value<std::string>(), "path to history file")

        ("stage", po::value<std::string>()->default_value("complete"), "Request query processing up to specified stage: complete,fetch_columns,with_mergeable_state,with_mergeable_state_after_aggregation,with_mergeable_state_after_aggregation_and_limit")
        ("progress", po::value<ProgressOption>()->implicit_value(ProgressOption::TTY, "tty")->default_value(ProgressOption::DEFAULT, "default"), "Print progress of queries execution - to TTY: tty|on|1|true|yes; to STDERR non-interactive mode: err; OFF: off|0|false|no; DEFAULT - interactive to TTY, non-interactive is off")

        ("disable_suggestion,A", "Disable loading suggestion data. Note that suggestion data is loaded asynchronously through a second connection to ClickHouse server. Also it is reasonable to disable suggestion if you want to paste a query with TAB characters. Shorthand option -A is for those who get used to mysql client.")
        ("wait_for_suggestions_to_load", "Load suggestion data synchonously.")
        ("time,t", "print query execution time to stderr in non-interactive mode (for benchmarks)")

        ("echo", "in batch mode, print query before execution")

        ("log-level", po::value<std::string>(), "log level")
        ("server_logs_file", po::value<std::string>(), "put server logs into specified file")

        ("suggestion_limit", po::value<int>()->default_value(10000), "Suggestion limit for how many databases, tables and columns to fetch.")

        ("format,f", po::value<std::string>(), "default output format (and input format for clickhouse-local)")
        ("output-format", po::value<std::string>(), "default output format (this option has preference over --format)")

        ("vertical,E", "vertical output format, same as --format=Vertical or FORMAT Vertical or \\G at end of command")
        ("highlight", po::value<bool>()->default_value(true), "enable or disable basic syntax highlight in interactive command line")

        ("ignore-error", "do not stop processing in multiquery mode")
        ("stacktrace", "print stack traces of exceptions")
        ("hardware-utilization", "print hardware utilization information in progress bar")
        ("print-profile-events", po::value(&profile_events.print)->zero_tokens(), "Printing ProfileEvents packets")
        ("profile-events-delay-ms", po::value<UInt64>()->default_value(profile_events.delay_ms), "Delay between printing `ProfileEvents` packets (-1 - print only totals, 0 - print every single packet)")
        ("processed-rows", "print the number of locally processed rows")

        ("interactive", "Process queries-file or --query query and start interactive mode")
        ("pager", po::value<std::string>(), "Pipe all output into this command (less or similar)")
        ("max_memory_usage_in_client", po::value<std::string>(), "Set memory limit in client/local server")

        ("fuzzer-args", po::value<std::string>(), "Command line arguments for the LLVM's libFuzzer driver. Only relevant if the application is compiled with libFuzzer.")
    ;

    addOptions(options_description);

    OptionsDescription options_description_non_verbose = options_description;

    auto getter = [](const auto & op)
    {
        String op_long_name = op->long_name();
        return "--" + String(op_long_name);
    };

    if (options_description.main_description)
    {
        const auto & main_options = options_description.main_description->options();
        std::transform(main_options.begin(), main_options.end(), std::back_inserter(cmd_options), getter);
    }

    if (options_description.external_description)
    {
        const auto & external_options = options_description.external_description->options();
        std::transform(external_options.begin(), external_options.end(), std::back_inserter(cmd_options), getter);
    }

    po::variables_map options;
    parseAndCheckOptions(options_description, options, common_arguments);
    po::notify(options);

    if (options.count("version") || options.count("V"))
    {
        showClientVersion();
        exit(0); // NOLINT(concurrency-mt-unsafe)
    }

    if (options.count("version-clean"))
    {
        std::cout << VERSION_STRING;
        exit(0); // NOLINT(concurrency-mt-unsafe)
    }

    if (options.count("verbose"))
        config().setBool("verbose", true);

    /// Output of help message.
    if (options.count("help")
        || (options.count("host") && options["host"].as<std::string>() == "elp")) /// If user writes -help instead of --help.
    {
        if (config().getBool("verbose", false))
            printHelpMessage(options_description, true);
        else
            printHelpMessage(options_description_non_verbose, false);
        exit(0); // NOLINT(concurrency-mt-unsafe)
    }

    /// Common options for clickhouse-client and clickhouse-local.
    if (options.count("time"))
        print_time_to_stderr = true;
    if (options.count("query"))
        queries = options["query"].as<std::vector<std::string>>();
    if (options.count("query_id"))
        config().setString("query_id", options["query_id"].as<std::string>());
    if (options.count("database"))
        config().setString("database", options["database"].as<std::string>());
    if (options.count("config-file"))
        config().setString("config-file", options["config-file"].as<std::string>());
    if (options.count("queries-file"))
        queries_files = options["queries-file"].as<std::vector<std::string>>();
    if (options.count("multiline"))
        config().setBool("multiline", true);
    if (options.count("multiquery"))
        config().setBool("multiquery", true);
    if (options.count("ignore-error"))
        config().setBool("ignore-error", true);
    if (options.count("format"))
        config().setString("format", options["format"].as<std::string>());
    if (options.count("output-format"))
        config().setString("output-format", options["output-format"].as<std::string>());
    if (options.count("vertical"))
        config().setBool("vertical", true);
    if (options.count("stacktrace"))
        config().setBool("stacktrace", true);
    if (options.count("print-profile-events"))
        config().setBool("print-profile-events", true);
    if (options.count("profile-events-delay-ms"))
        config().setUInt64("profile-events-delay-ms", options["profile-events-delay-ms"].as<UInt64>());
    if (options.count("processed-rows"))
        print_num_processed_rows = true;
    if (options.count("progress"))
    {
        switch (options["progress"].as<ProgressOption>())
        {
            case DEFAULT:
                config().setString("progress", "default");
                break;
            case OFF:
                config().setString("progress", "off");
                break;
            case TTY:
                config().setString("progress", "tty");
                break;
            case ERR:
                config().setString("progress", "err");
                break;
        }
    }
    if (options.count("echo"))
        config().setBool("echo", true);
    if (options.count("disable_suggestion"))
        config().setBool("disable_suggestion", true);
    if (options.count("wait_for_suggestions_to_load"))
        config().setBool("wait_for_suggestions_to_load", true);
    if (options.count("suggestion_limit"))
        config().setInt("suggestion_limit", options["suggestion_limit"].as<int>());
    if (options.count("highlight"))
        config().setBool("highlight", options["highlight"].as<bool>());
    if (options.count("history_file"))
        config().setString("history_file", options["history_file"].as<std::string>());
    if (options.count("interactive"))
        config().setBool("interactive", true);
    if (options.count("pager"))
        config().setString("pager", options["pager"].as<std::string>());

    if (options.count("log-level"))
        Poco::Logger::root().setLevel(options["log-level"].as<std::string>());
    if (options.count("server_logs_file"))
        server_logs_file = options["server_logs_file"].as<std::string>();

    query_processing_stage = QueryProcessingStage::fromString(options["stage"].as<std::string>());
    query_kind = parseQueryKind(options["query_kind"].as<std::string>());
    profile_events.print = options.count("print-profile-events");
    profile_events.delay_ms = options["profile-events-delay-ms"].as<UInt64>();

    processOptions(options_description, options, external_tables_arguments, hosts_and_ports_arguments);
    {
        std::unordered_set<std::string> alias_names;
        alias_names.reserve(options_description.main_description->options().size());
        for (const auto& option : options_description.main_description->options())
            alias_names.insert(option->long_name());
        argsToConfig(common_arguments, config(), 100, &alias_names);
    }

    clearPasswordFromCommandLine(argc, argv);

    /// Limit on total memory usage
    std::string max_client_memory_usage = config().getString("max_memory_usage_in_client", "0" /*default value*/);
    if (max_client_memory_usage != "0")
    {
        UInt64 max_client_memory_usage_int = parseWithSizeSuffix<UInt64>(max_client_memory_usage.c_str(), max_client_memory_usage.length());

        total_memory_tracker.setHardLimit(max_client_memory_usage_int);
        total_memory_tracker.setDescription("(total)");
        total_memory_tracker.setMetric(CurrentMetrics::MemoryTracking);
    }

    has_log_comment = config().has("log_comment");
}

}
