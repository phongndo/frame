set positional-arguments

nix := ""
profile := "debug"
build_type := if profile == "release" { "Release" } else if profile == "dev" { "Dev" } else { "Debug" }
conan_build_type := if profile == "dev" { "Release" } else { build_type }
cpp_files := "apps include src tests benchmarks fuzz"
python_paths := "bench benchmarks extensions scripts test tests tools conanfile.py"
ghostty_cmake := if env_var_or_default("LEMMA_GHOSTTY_SOURCE_DIR", "") != "" { "-DLEMMA_GHOSTTY_SOURCE_DIR=" + env_var("LEMMA_GHOSTTY_SOURCE_DIR") + " -DLEMMA_GHOSTTY_NIX_SOURCE_REV=" + env_var_or_default("LEMMA_GHOSTTY_NIX_SOURCE_REV", "") + " -DLEMMA_GHOSTTY_ZIG_SYSTEM_DIR=" + env_var_or_default("LEMMA_GHOSTTY_ZIG_SYSTEM_DIR", "") } else { "" }

_default:
    @just --list

# Show the pinned development tool versions.
versions:
    {{ nix }} clang++ --version
    {{ nix }} clangd --version
    {{ nix }} clang-tidy --version
    {{ nix }} cmake --version
    {{ nix }} ninja --version
    {{ nix }} ccache --version
    {{ nix }} conan --version
    {{ nix }} zig version
    {{ nix }} hk --version
    {{ nix }} uv --version
    {{ nix }} uv run --locked ruff --version
    {{ nix }} uv run --locked ty --version

# Show versions of the optional multiplexer benchmark subjects.
benchmark-versions:
    {{ nix }} tmux -V
    {{ nix }} zellij --version
    {{ nix }} herdr --version

# Install Conan dependencies for the selected profile.
deps:
    rm -f CMakeUserPresets.json
    {{ nix }} conan install . \
        --output-folder=build/{{ profile }}/conan \
        --lockfile=conan.lock \
        --profile:all=conan/profiles/llvm \
        --settings=build_type={{ conan_build_type }} \
        --conf=tools.cmake.cmaketoolchain:user_presets= \
        --build=missing

# Generate Ninja files and compile_commands.json.
configure: deps
    {{ nix }} cmake --preset {{ profile }} \
        -DCMAKE_BUILD_TYPE={{ build_type }} \
        -DCMAKE_TOOLCHAIN_FILE="$PWD/build/{{ profile }}/conan/conan_toolchain.cmake" \
        {{ ghostty_cmake }} \
        -DLEMMA_BUILD_TESTS=ON -DLEMMA_BUILD_BENCHMARKS=ON

# Build the application, tests, and benchmarks.
build: configure
    {{ nix }} cmake --build --preset {{ profile }}

# Incrementally build and run the isolated current-checkout development application.
run *args:
    @exec ./scripts/dev-run "$@"

# Run the scripted libghostty-vt demo without adding a production CLI command.
demo: build
    {{ nix }} ./build/{{ profile }}/lemma_test_cli /tmp/lemma-demo-unused.sock demo

# Run the fast native and real-mux developer suite (stress/extended remain explicit).
test:
    {{ nix }} LEMMA_TEST_PROFILE={{ profile }} ./test

# Run the short Release native benchmark smoke (use ./bench <domain> for filtering).
bench:
    {{ nix }} LEMMA_BENCH_PROFILE=release ./bench

# Run release native and direct/Lemma/tmux/Zellij/Herdr process baselines.
mux-bench:
    {{ nix }} scripts/ci/benchmarks extended

# Compare coding-agent behavior with and without the embedded Lemma skill.
skill-bench *args:
    {{ nix }} uv run --locked python tools/benchmark_lemma_skill.py "$@"

# Prove regression detectors reject isolated production-source faults (opt-in, not timing evidence).
detection-check *args:
    {{ nix }} uv run --locked python tools/validate_detection.py "$@"

# Enforce the reviewed performance budgets on the pinned dedicated host.
regression-bench:
    {{ nix }} scripts/ci/regression-budgets

# Manually compare BASELINE with the current checkout on the approved local host.
performance-gate baseline="HEAD" output="":
    {{ nix }} scripts/performance gate "{{ baseline }}" "{{ output }}"

# Measure repeated same-revision noise on the approved local host.
performance-calibrate captures="3" output="":
    {{ nix }} scripts/performance calibrate "{{ captures }}" "{{ output }}"

# Compare the current Release build with extensions off and under reviewed external workloads.
performance-extension output="":
    {{ nix }} scripts/performance extension "{{ output }}"

# Format C++, Nix, and Python files in place.
fmt:
    {{ nix }} bash -c "find {{ cpp_files }} -type f \
        \\( -name '*.cpp' -o -name '*.hpp' \\) -print0 | xargs -0 clang-format -i"
    {{ nix }} nixpkgs-fmt flake.nix
    {{ nix }} uv run --locked ruff check --fix {{ python_paths }}
    {{ nix }} uv run --locked ruff format {{ python_paths }}

# Check formatting without changing files.
fmt-check:
    {{ nix }} bash -c "find {{ cpp_files }} -type f \
        \\( -name '*.cpp' -o -name '*.hpp' \\) -print0 | \
        xargs -0 clang-format --dry-run --Werror"
    {{ nix }} nixpkgs-fmt --check flake.nix
    {{ nix }} uv run --locked ruff format --check {{ python_paths }}

_analysis-inputs: configure
    {{ nix }} cmake --build --preset {{ profile }} --target lemma_analysis_inputs

# Run responsive clang-tidy checks in parallel; ci-lint adds the slower Static Analyzer.
lint: _analysis-inputs
    {{ nix }} bash -c "find apps src tests benchmarks -type f -name '*.cpp' -print0 | \
        xargs -0 -n 1 -P \"\${CLANG_TIDY_JOBS:-4}\" \
        clang-tidy --quiet -p build/{{ profile }}"

# Check every public header and production translation unit through clangd in parallel.
lsp-check: _analysis-inputs
    {{ nix }} bash -c "find apps include src -type f \
        \\( -name '*.hpp' -o -name '*.cpp' \\) -print0 | sort -z | \
        xargs -0 -n 1 -P \"\${CLANGD_JOBS:-4}\" cmake/check-clangd.sh"

# Start clangd for editor integrations.
lsp:
    {{ nix }} clangd --enable-config

# Run Ruff, ty, and Python unit tests.
python-check:
    {{ nix }} scripts/ci/python

# Check local documentation links, catalogs, schemas, and canonical example snippets.
docs-check:
    {{ nix }} uv run --locked python -m unittest tools.test_docs tools.test_api_schema
    {{ nix }} uv run --locked python tools/check_docs.py

# Run documentation checks, formatting, lint, LSP diagnostics, build, and tests.
check: docs-check build fmt-check lint lsp-check test python-check

# Check the merge-blocking formatter lane.
ci-format:
    {{ nix }} scripts/ci/format

# Run the merge-blocking debug build and test lane.
ci-build-test:
    {{ nix }} scripts/ci/build-test

# Run the merge-blocking clang-tidy lane.
ci-lint:
    {{ nix }} scripts/ci/lint

# Run the merge-blocking clangd lane.
ci-lsp:
    {{ nix }} scripts/ci/lsp

# Run the merge-blocking Python quality lane.
ci-python:
    {{ nix }} scripts/ci/python

# Run the regular C++ lanes sequentially for local reproduction.
ci-cpp:
    {{ nix }} scripts/ci/cpp

# Run the scheduled/local release benchmark smoke lane.
ci-benchmarks:
    {{ nix }} scripts/ci/benchmarks smoke

# Reproduce the sanitizer suite (the merge-blocking hosted lane uses Linux).
ci-sanitizers:
    {{ nix }} scripts/ci/sanitizers

# Run the CI change-classification contract tests.
ci-contracts:
    {{ nix }} scripts/ci/contracts

# Run ShellCheck over CI scripts.
ci-shellcheck:
    {{ nix }} scripts/ci/shellcheck

# Run Actionlint over GitHub Actions workflows.
ci-actionlint:
    {{ nix }} scripts/ci/actionlint

# Run all automation checks sequentially for local reproduction.
ci-workflows:
    {{ nix }} scripts/ci/workflows

# Reproduce every merge-blocking CI lane locally in a safe sequence.
ci-check: docs-check
    {{ nix }} scripts/ci/cpp
    {{ nix }} scripts/ci/sanitizers
    {{ nix }} scripts/ci/python
    {{ nix }} scripts/ci/workflows

# Configure the debug tree and install this repository's hk hooks.
hooks:
    {{ nix }} bash -c 'scripts/ci/configure debug -DLEMMA_BUILD_TESTS=ON -DLEMMA_BUILD_BENCHMARKS=ON && hk validate && hk install'

# Run every fast and pre-push hk check over the repository.
hooks-check:
    {{ nix }} hk check --all --check

# Apply every safe pre-commit-tier hk fixer.
hooks-fix:
    {{ nix }} hk fix --all

# Remove generated build and vendored Zig output while preserving the shared compiler cache.
clean:
    rm -rf build CMakeUserPresets.json .zig-cache \
        third_party/ghostty/.zig-cache third_party/ghostty/zig-out third_party/ghostty/zig-pkg

# Explicitly clear the compiler cache shared by every worktree.
clean-cache:
    {{ nix }} ccache --clear
