import random
import re
from argparse import ArgumentDefaultsHelpFormatter, ArgumentParser
from typing import Dict, Optional, List

from ci_utils import normalize_string
from ci_definitions import *


class CI:
    """
    Contains configs for all jobs in the CI pipeline
    each config item in the below dicts should be an instance of JobConfig class or inherited from it
    """

    # reimport types to CI class so that they visible as CI.* and mypy is happy
    # pylint:disable=useless-import-alias,reimported,import-outside-toplevel
    from ci_definitions import BuildConfig as BuildConfig
    from ci_definitions import DigestConfig as DigestConfig
    from ci_definitions import JobConfig as JobConfig
    from ci_definitions import CheckDescription as CheckDescription
    from ci_definitions import Tags as Tags
    from ci_definitions import JobNames as JobNames
    from ci_definitions import BuildNames as BuildNames
    from ci_definitions import StatusNames as StatusNames
    from ci_definitions import CHECK_DESCRIPTIONS as CHECK_DESCRIPTIONS
    from ci_definitions import REQUIRED_CHECKS as REQUIRED_CHECKS
    from ci_definitions import SyncState as SyncState
    from ci_definitions import MQ_JOBS as MQ_JOBS
    from ci_definitions import WorkflowStages as WorkflowStages
    from ci_definitions import Runners as Runners

    # Jobs that run for doc related updates
    _DOCS_CHECK_JOBS = [JobNames.DOCS_CHECK, JobNames.STYLE_CHECK]

    TAG_CONFIGS = {
        Tags.DO_NOT_TEST_LABEL: LabelConfig(run_jobs=[JobNames.STYLE_CHECK]),
        Tags.CI_SET_ARM: LabelConfig(
            run_jobs=[
                JobNames.STYLE_CHECK,
                BuildNames.PACKAGE_AARCH64,
                JobNames.INTEGRATION_TEST_ARM,
            ]
        ),
        Tags.CI_SET_REQUIRED: LabelConfig(run_jobs=REQUIRED_CHECKS),
        Tags.CI_SET_BUILDS: LabelConfig(
            run_jobs=[JobNames.STYLE_CHECK, JobNames.BUILD_CHECK]
            + [build for build in BuildNames if build != BuildNames.FUZZERS]
        ),
        Tags.CI_SET_NON_REQUIRED: LabelConfig(
            run_jobs=[job for job in JobNames if job not in REQUIRED_CHECKS]
        ),
        Tags.CI_SET_OLD_ANALYZER: LabelConfig(
            run_jobs=[
                JobNames.STYLE_CHECK,
                JobNames.FAST_TEST,
                BuildNames.PACKAGE_RELEASE,
                BuildNames.PACKAGE_ASAN,
                JobNames.STATELESS_TEST_OLD_ANALYZER_S3_REPLICATED_RELEASE,
                JobNames.INTEGRATION_TEST_ASAN_OLD_ANALYZER,
            ]
        ),
        Tags.CI_SET_SYNC: LabelConfig(
            run_jobs=[
                BuildNames.PACKAGE_ASAN,
                JobNames.STYLE_CHECK,
                JobNames.BUILD_CHECK,
                JobNames.UNIT_TEST_ASAN,
                JobNames.STATEFUL_TEST_ASAN,
            ]
        ),
    }

    JOB_CONFIGS: Dict[str, JobConfig] = {
        BuildNames.PACKAGE_RELEASE: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.PACKAGE_RELEASE,
                compiler="clang-18",
                package_type="deb",
                static_binary_name="amd64",
                additional_pkgs=True,
            )
        ),
        BuildNames.PACKAGE_AARCH64: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.PACKAGE_AARCH64,
                compiler="clang-18-aarch64",
                package_type="deb",
                static_binary_name="aarch64",
                additional_pkgs=True,
            )
        ),
        BuildNames.PACKAGE_ASAN: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.PACKAGE_ASAN,
                compiler="clang-18",
                sanitizer="address",
                package_type="deb",
            ),
        ),
        BuildNames.PACKAGE_UBSAN: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.PACKAGE_UBSAN,
                compiler="clang-18",
                sanitizer="undefined",
                package_type="deb",
            ),
        ),
        BuildNames.PACKAGE_TSAN: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.PACKAGE_TSAN,
                compiler="clang-18",
                sanitizer="thread",
                package_type="deb",
            ),
        ),
        BuildNames.PACKAGE_MSAN: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.PACKAGE_MSAN,
                compiler="clang-18",
                sanitizer="memory",
                package_type="deb",
            ),
        ),
        BuildNames.PACKAGE_DEBUG: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.PACKAGE_DEBUG,
                compiler="clang-18",
                debug_build=True,
                package_type="deb",
                sparse_checkout=True,  # Check that it works with at least one build, see also update-submodules.sh
            ),
        ),
        BuildNames.PACKAGE_RELEASE_COVERAGE: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.PACKAGE_RELEASE_COVERAGE,
                compiler="clang-18",
                coverage=True,
                package_type="deb",
            ),
        ),
        BuildNames.BINARY_RELEASE: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.BINARY_RELEASE,
                compiler="clang-18",
                package_type="binary",
            ),
        ),
        BuildNames.BINARY_TIDY: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.BINARY_TIDY,
                compiler="clang-18",
                debug_build=True,
                package_type="binary",
                static_binary_name="debug-amd64",
                tidy=True,
                comment="clang-tidy is used for static analysis",
            ),
        ),
        BuildNames.BINARY_DARWIN: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.BINARY_DARWIN,
                compiler="clang-18-darwin",
                package_type="binary",
                static_binary_name="macos",
            ),
        ),
        BuildNames.BINARY_AARCH64: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.BINARY_AARCH64,
                compiler="clang-18-aarch64",
                package_type="binary",
            ),
        ),
        BuildNames.BINARY_AARCH64_V80COMPAT: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.BINARY_AARCH64_V80COMPAT,
                compiler="clang-18-aarch64-v80compat",
                package_type="binary",
                static_binary_name="aarch64v80compat",
                comment="For ARMv8.1 and older",
            ),
        ),
        BuildNames.BINARY_FREEBSD: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.BINARY_FREEBSD,
                compiler="clang-18-freebsd",
                package_type="binary",
                static_binary_name="freebsd",
            ),
        ),
        BuildNames.BINARY_DARWIN_AARCH64: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.BINARY_DARWIN_AARCH64,
                compiler="clang-18-darwin-aarch64",
                package_type="binary",
                static_binary_name="macos-aarch64",
            ),
        ),
        BuildNames.BINARY_PPC64LE: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.BINARY_PPC64LE,
                compiler="clang-18-ppc64le",
                package_type="binary",
                static_binary_name="powerpc64le",
            ),
        ),
        BuildNames.BINARY_AMD64_COMPAT: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.BINARY_AMD64_COMPAT,
                compiler="clang-18-amd64-compat",
                package_type="binary",
                static_binary_name="amd64compat",
                comment="SSE2-only build",
            ),
        ),
        BuildNames.BINARY_AMD64_MUSL: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.BINARY_AMD64_MUSL,
                compiler="clang-18-amd64-musl",
                package_type="binary",
                static_binary_name="amd64musl",
                comment="Build with Musl",
            ),
        ),
        BuildNames.BINARY_RISCV64: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.BINARY_RISCV64,
                compiler="clang-18-riscv64",
                package_type="binary",
                static_binary_name="riscv64",
            ),
        ),
        BuildNames.BINARY_S390X: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.BINARY_S390X,
                compiler="clang-18-s390x",
                package_type="binary",
                static_binary_name="s390x",
            ),
        ),
        BuildNames.BINARY_LOONGARCH64: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.BINARY_LOONGARCH64,
                compiler="clang-18-loongarch64",
                package_type="binary",
                static_binary_name="loongarch64",
            ),
        ),
        BuildNames.FUZZERS: CommonJobConfigs.BUILD.with_properties(
            build_config=BuildConfig(
                name=BuildNames.FUZZERS,
                compiler="clang-18",
                package_type="fuzzers",
            ),
            run_by_label=Tags.libFuzzer,
        ),
        JobNames.BUILD_CHECK: CommonJobConfigs.BUILD_REPORT.with_properties(),
        JobNames.INSTALL_TEST_AMD: CommonJobConfigs.INSTALL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE]
        ),
        JobNames.INSTALL_TEST_ARM: CommonJobConfigs.INSTALL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_AARCH64],
            runner_type=Runners.STYLE_CHECKER_ARM,
        ),
        JobNames.STATEFUL_TEST_ASAN: CommonJobConfigs.STATEFUL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_ASAN]
        ),
        JobNames.STATEFUL_TEST_TSAN: CommonJobConfigs.STATEFUL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_TSAN]
        ),
        JobNames.STATEFUL_TEST_MSAN: CommonJobConfigs.STATEFUL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_MSAN]
        ),
        JobNames.STATEFUL_TEST_UBSAN: CommonJobConfigs.STATEFUL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_UBSAN]
        ),
        JobNames.STATEFUL_TEST_DEBUG: CommonJobConfigs.STATEFUL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_DEBUG]
        ),
        JobNames.STATEFUL_TEST_RELEASE: CommonJobConfigs.STATEFUL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE]
        ),
        JobNames.STATEFUL_TEST_RELEASE_COVERAGE: CommonJobConfigs.STATEFUL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE_COVERAGE]
        ),
        JobNames.STATEFUL_TEST_AARCH64: CommonJobConfigs.STATEFUL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_AARCH64],
            runner_type=Runners.FUNC_TESTER_ARM,
        ),
        JobNames.STATEFUL_TEST_PARALLEL_REPL_RELEASE: CommonJobConfigs.STATEFUL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE]
        ),
        JobNames.STATEFUL_TEST_PARALLEL_REPL_DEBUG: CommonJobConfigs.STATEFUL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_DEBUG]
        ),
        JobNames.STATEFUL_TEST_PARALLEL_REPL_ASAN: CommonJobConfigs.STATEFUL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_ASAN],
            random_bucket="parrepl_with_sanitizer",
        ),
        JobNames.STATEFUL_TEST_PARALLEL_REPL_MSAN: CommonJobConfigs.STATEFUL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_MSAN],
            random_bucket="parrepl_with_sanitizer",
        ),
        JobNames.STATEFUL_TEST_PARALLEL_REPL_UBSAN: CommonJobConfigs.STATEFUL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_UBSAN],
            random_bucket="parrepl_with_sanitizer",
        ),
        JobNames.STATEFUL_TEST_PARALLEL_REPL_TSAN: CommonJobConfigs.STATEFUL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_TSAN],
            random_bucket="parrepl_with_sanitizer",
        ),
        JobNames.STATELESS_TEST_ASAN: CommonJobConfigs.STATELESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_ASAN], num_batches=4
        ),
        JobNames.STATELESS_TEST_TSAN: CommonJobConfigs.STATELESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_TSAN], num_batches=5
        ),
        JobNames.STATELESS_TEST_MSAN: CommonJobConfigs.STATELESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_MSAN], num_batches=6
        ),
        JobNames.STATELESS_TEST_UBSAN: CommonJobConfigs.STATELESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_UBSAN], num_batches=2
        ),
        JobNames.STATELESS_TEST_DEBUG: CommonJobConfigs.STATELESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_DEBUG], num_batches=5
        ),
        JobNames.STATELESS_TEST_RELEASE: CommonJobConfigs.STATELESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE],
        ),
        JobNames.STATELESS_TEST_RELEASE_COVERAGE: CommonJobConfigs.STATELESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE_COVERAGE], num_batches=6
        ),
        JobNames.STATELESS_TEST_AARCH64: CommonJobConfigs.STATELESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_AARCH64],
            runner_type=Runners.FUNC_TESTER_ARM,
        ),
        JobNames.STATELESS_TEST_OLD_ANALYZER_S3_REPLICATED_RELEASE: CommonJobConfigs.STATELESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE], num_batches=4
        ),
        JobNames.STATELESS_TEST_S3_DEBUG: CommonJobConfigs.STATELESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_DEBUG], num_batches=6
        ),
        JobNames.STATELESS_TEST_AZURE_ASAN: CommonJobConfigs.STATELESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_ASAN], num_batches=4, release_only=True
        ),
        JobNames.STATELESS_TEST_S3_TSAN: CommonJobConfigs.STATELESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_TSAN],
            num_batches=5,
        ),
        JobNames.STRESS_TEST_DEBUG: CommonJobConfigs.STRESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_DEBUG],
        ),
        JobNames.STRESS_TEST_TSAN: CommonJobConfigs.STRESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_TSAN],
        ),
        JobNames.STRESS_TEST_ASAN: CommonJobConfigs.STRESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_ASAN],
            random_bucket="stress_with_sanitizer",
        ),
        JobNames.STRESS_TEST_UBSAN: CommonJobConfigs.STRESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_UBSAN],
            random_bucket="stress_with_sanitizer",
        ),
        JobNames.STRESS_TEST_MSAN: CommonJobConfigs.STRESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_MSAN],
            random_bucket="stress_with_sanitizer",
        ),
        JobNames.STRESS_TEST_AZURE_TSAN: CommonJobConfigs.STRESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_TSAN], release_only=True
        ),
        JobNames.STRESS_TEST_AZURE_MSAN: CommonJobConfigs.STRESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_MSAN], release_only=True
        ),
        JobNames.UPGRADE_TEST_ASAN: CommonJobConfigs.UPGRADE_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_ASAN],
            random_bucket="upgrade_with_sanitizer",
            pr_only=True,
        ),
        JobNames.UPGRADE_TEST_TSAN: CommonJobConfigs.UPGRADE_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_TSAN],
            random_bucket="upgrade_with_sanitizer",
            pr_only=True,
        ),
        JobNames.UPGRADE_TEST_MSAN: CommonJobConfigs.UPGRADE_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_MSAN],
            random_bucket="upgrade_with_sanitizer",
            pr_only=True,
        ),
        JobNames.UPGRADE_TEST_DEBUG: CommonJobConfigs.UPGRADE_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_DEBUG], pr_only=True
        ),
        JobNames.INTEGRATION_TEST_ASAN: CommonJobConfigs.INTEGRATION_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_ASAN], release_only=True, num_batches=4
        ),
        JobNames.INTEGRATION_TEST_ASAN_OLD_ANALYZER: CommonJobConfigs.INTEGRATION_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_ASAN], num_batches=6
        ),
        JobNames.INTEGRATION_TEST_TSAN: CommonJobConfigs.INTEGRATION_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_TSAN], num_batches=6
        ),
        JobNames.INTEGRATION_TEST_ARM: CommonJobConfigs.INTEGRATION_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_AARCH64],
            num_batches=6,
            runner_type=Runners.FUNC_TESTER_ARM,
        ),
        JobNames.INTEGRATION_TEST: CommonJobConfigs.INTEGRATION_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE],
            num_batches=4,
            release_only=True,
        ),
        JobNames.INTEGRATION_TEST_FLAKY: CommonJobConfigs.INTEGRATION_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_ASAN], pr_only=True
        ),
        JobNames.COMPATIBILITY_TEST: CommonJobConfigs.COMPATIBILITY_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE],
            required_on_release_branch=True,
        ),
        JobNames.COMPATIBILITY_TEST_ARM: CommonJobConfigs.COMPATIBILITY_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_AARCH64],
            required_on_release_branch=True,
            runner_type=Runners.STYLE_CHECKER_ARM,
        ),
        JobNames.UNIT_TEST: CommonJobConfigs.UNIT_TEST.with_properties(
            required_builds=[BuildNames.BINARY_RELEASE],
        ),
        JobNames.UNIT_TEST_ASAN: CommonJobConfigs.UNIT_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_ASAN],
        ),
        JobNames.UNIT_TEST_MSAN: CommonJobConfigs.UNIT_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_MSAN],
        ),
        JobNames.UNIT_TEST_TSAN: CommonJobConfigs.UNIT_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_TSAN],
        ),
        JobNames.UNIT_TEST_UBSAN: CommonJobConfigs.UNIT_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_UBSAN],
        ),
        JobNames.AST_FUZZER_TEST_DEBUG: CommonJobConfigs.ASTFUZZER_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_DEBUG],
        ),
        JobNames.AST_FUZZER_TEST_ASAN: CommonJobConfigs.ASTFUZZER_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_ASAN],
        ),
        JobNames.AST_FUZZER_TEST_MSAN: CommonJobConfigs.ASTFUZZER_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_MSAN],
        ),
        JobNames.AST_FUZZER_TEST_TSAN: CommonJobConfigs.ASTFUZZER_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_TSAN],
        ),
        JobNames.AST_FUZZER_TEST_UBSAN: CommonJobConfigs.ASTFUZZER_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_UBSAN],
        ),
        JobNames.STATELESS_TEST_FLAKY_ASAN: CommonJobConfigs.STATELESS_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_ASAN], pr_only=True, timeout=3600
        ),
        JobNames.JEPSEN_KEEPER: JobConfig(
            required_builds=[BuildNames.BINARY_RELEASE],
            run_by_label="jepsen-test",
            run_command="jepsen_check.py keeper",
            runner_type=Runners.STYLE_CHECKER_ARM,
        ),
        JobNames.JEPSEN_SERVER: JobConfig(
            required_builds=[BuildNames.BINARY_RELEASE],
            run_by_label="jepsen-test",
            run_command="jepsen_check.py server",
            runner_type=Runners.STYLE_CHECKER_ARM,
        ),
        JobNames.PERFORMANCE_TEST_AMD64: CommonJobConfigs.PERF_TESTS.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE], num_batches=4
        ),
        JobNames.PERFORMANCE_TEST_ARM64: CommonJobConfigs.PERF_TESTS.with_properties(
            required_builds=[BuildNames.PACKAGE_AARCH64],
            num_batches=4,
            run_by_label="pr-performance",
            runner_type=Runners.FUNC_TESTER_ARM,
        ),
        JobNames.SQLANCER: CommonJobConfigs.SQLLANCER_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE],
        ),
        JobNames.SQLANCER_DEBUG: CommonJobConfigs.SQLLANCER_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_DEBUG],
        ),
        JobNames.SQL_LOGIC_TEST: CommonJobConfigs.SQLLOGIC_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE],
        ),
        JobNames.SQLTEST: CommonJobConfigs.SQL_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE],
        ),
        JobNames.CLICKBENCH_TEST: CommonJobConfigs.CLICKBENCH_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE],
        ),
        JobNames.CLICKBENCH_TEST_ARM: CommonJobConfigs.CLICKBENCH_TEST.with_properties(
            required_builds=[BuildNames.PACKAGE_AARCH64],
            runner_type=Runners.FUNC_TESTER_ARM,
        ),
        JobNames.LIBFUZZER_TEST: JobConfig(
            required_builds=[BuildNames.FUZZERS],
            run_by_label=Tags.libFuzzer,
            timeout=10800,
            run_command='libfuzzer_test_check.py "$CHECK_NAME"',
            runner_type=Runners.STYLE_CHECKER,
        ),
        JobNames.DOCKER_SERVER: CommonJobConfigs.DOCKER_SERVER.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE]
        ),
        JobNames.DOCKER_KEEPER: CommonJobConfigs.DOCKER_SERVER.with_properties(
            required_builds=[BuildNames.PACKAGE_RELEASE]
        ),
        JobNames.DOCS_CHECK: JobConfig(
            digest=DigestConfig(
                include_paths=["**/*.md", "./docs", "tests/ci/docs_check.py"],
                docker=["clickhouse/docs-builder"],
            ),
            run_command="docs_check.py",
            runner_type=Runners.FUNC_TESTER,
        ),
        JobNames.FAST_TEST: JobConfig(
            pr_only=True,
            digest=DigestConfig(
                include_paths=["./tests/queries/0_stateless/"],
                exclude_files=[".md"],
                docker=["clickhouse/fasttest"],
            ),
            timeout=2400,
            runner_type=Runners.BUILDER,
        ),
        JobNames.STYLE_CHECK: JobConfig(
            run_always=True,
            runner_type=Runners.STYLE_CHECKER_ARM,
        ),
        JobNames.BUGFIX_VALIDATE: JobConfig(
            run_by_label="pr-bugfix",
            run_command="bugfix_validate_check.py",
            timeout=900,
            runner_type=Runners.STYLE_CHECKER,
        ),
    }

    @classmethod
    def get_tag_config(cls, label_name: str) -> Optional[LabelConfig]:
        for label, config in cls.TAG_CONFIGS.items():
            if normalize_string(label_name) == normalize_string(label):
                return config
        return None

    @classmethod
    def get_job_ci_stage(cls, job_name: str) -> str:
        if job_name in [
            JobNames.STYLE_CHECK,
            JobNames.FAST_TEST,
            JobNames.JEPSEN_SERVER,
            JobNames.JEPSEN_KEEPER,
            JobNames.BUILD_CHECK,
        ]:
            return WorkflowStages.NA

        stage_type = None
        if cls.is_build_job(job_name):
            for _job, config in cls.JOB_CONFIGS.items():
                if config.required_builds and job_name in config.required_builds:
                    stage_type = WorkflowStages.BUILDS_1
                    break
            else:
                stage_type = WorkflowStages.BUILDS_2
        elif cls.is_docs_job(job_name):
            stage_type = WorkflowStages.TESTS_1
        elif cls.is_test_job(job_name):
            if job_name in CI.JOB_CONFIGS:
                if job_name in REQUIRED_CHECKS:
                    stage_type = WorkflowStages.TESTS_1
                else:
                    stage_type = WorkflowStages.TESTS_3
        assert stage_type, f"BUG [{job_name}]"
        return stage_type

    @classmethod
    def get_job_config(cls, check_name: str) -> JobConfig:
        return cls.JOB_CONFIGS[check_name]

    @classmethod
    def get_required_build_name(cls, check_name: str) -> str:
        assert check_name in cls.JOB_CONFIGS
        required_builds = cls.JOB_CONFIGS[check_name].required_builds
        assert required_builds and len(required_builds) == 1
        return required_builds[0]

    @classmethod
    def get_job_parents(cls, check_name: str) -> List[str]:
        return cls.JOB_CONFIGS[check_name].required_builds or []

    @classmethod
    def get_workflow_jobs_with_configs(
        cls, is_mq: bool, is_docs_only: bool, is_master: bool, is_pr: bool
    ) -> Dict[str, JobConfig]:
        """
        get a list of all jobs for a workflow with configs
        """
        jobs = []
        if is_mq:
            jobs = MQ_JOBS
        elif is_docs_only:
            jobs = cls._DOCS_CHECK_JOBS
        else:
            # add all jobs
            jobs = list(cls.JOB_CONFIGS)
            if is_master:
                for job in MQ_JOBS:
                    jobs.remove(job)

        randomization_bucket_jobs = {}  # type: Dict[str, Dict[str, JobConfig]]
        res = {}  # type: Dict[str, JobConfig]
        for job in jobs:
            job_config = cls.JOB_CONFIGS[job]

            if job_config.random_bucket and is_pr:
                if job_config.random_bucket not in randomization_bucket_jobs:
                    randomization_bucket_jobs[job_config.random_bucket] = {}
                randomization_bucket_jobs[job_config.random_bucket][job] = job_config
                continue

            res[job] = job_config

        # add to the result a random job from each random bucket, if any
        for bucket, jobs_configs in randomization_bucket_jobs.items():
            job = random.choice(list(jobs_configs))
            print(f"Pick job [{job}] from randomization bucket [{bucket}]")
            res[job] = jobs_configs[job]

        return res

    @classmethod
    def is_build_job(cls, job: str) -> bool:
        return job in cls.BuildNames

    @classmethod
    def is_test_job(cls, job: str) -> bool:
        return not cls.is_build_job(job) and job != cls.JobNames.STYLE_CHECK

    @classmethod
    def is_docs_job(cls, job: str) -> bool:
        return job == JobNames.DOCS_CHECK

    @classmethod
    def is_required(cls, check_name: str) -> bool:
        """Checks if a check_name is in REQUIRED_CHECKS, including batched jobs"""
        _BATCH_REGEXP = re.compile(r"\s+\[[0-9/]+\]$")
        if check_name in REQUIRED_CHECKS:
            return True
        if batch := _BATCH_REGEXP.search(check_name):
            return check_name[: batch.start()] in REQUIRED_CHECKS
        return False

    @classmethod
    def get_build_config(cls, build_name: str) -> BuildConfig:
        assert build_name in cls.JOB_CONFIGS, f"Invalid build name [{build_name}]"
        res = cls.JOB_CONFIGS[build_name].build_config
        assert res, f"not a build [{build_name}] or invalid JobConfig"
        return res


if __name__ == "__main__":
    parser = ArgumentParser(
        formatter_class=ArgumentDefaultsHelpFormatter,
        description="The script provides build config for GITHUB_ENV or shell export",
    )
    parser.add_argument("--build-name", help="the build config to export")
    parser.add_argument(
        "--export",
        action="store_true",
        help="if set, the ENV parameters are provided for shell export",
    )
    args = parser.parse_args()
    assert (
        args.build_name in CI.JOB_CONFIGS
    ), f"Build name [{args.build_name}] is not valid"
    build_config = CI.JOB_CONFIGS[args.build_name].build_config
    assert build_config, "--export must not be used for non-build jobs"
    print(build_config.export_env(args.export))
