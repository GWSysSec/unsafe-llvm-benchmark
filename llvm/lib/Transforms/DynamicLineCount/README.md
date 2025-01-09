# dynamic line count pass for rust

an llvm pass for analyzing unsafe code patterns in rust programs.

## detection logic

the pass works by instrumenting source locations during compilation. it captures every instruction's debug location and determines its safety status. if an instruction has the `unsafe_inst` metadata, it is marked as unsafe. debug locations help map instructions to source lines in the code. compiler-generated instructions or lines from rust standard libraries are skipped.

## counting logic

for each unique source location, the pass inserts counters into the compiled code. these counters track:

- whether a line of code is executed during runtime
- whether a line contains unsafe instructions

the runtime buffers updates in thread-local storage and periodically merges them into a global structure. the counters are flushed to disk when the program exits.

## runtime integration

the runtime is responsible for managing line execution data. it:

1. initializes a global state to store coverage statistics.
2. uses thread-local buffers to batch updates and avoid contention.
3. processes and aggregates data at program termination to generate a detailed safety report.

the runtime exposes apis that the instrumented code calls, such as `update_line_counter` and `mark_line_executed`. these apis handle both unsafe and normal lines efficiently, ensuring minimal runtime overhead.

## key points

- works seamlessly with rust projects by modifying `rustflags` in `.cargo/config.toml`.
- integrates tightly with llvm to provide precise and actionable insights.
- generates safety reports highlighting unsafe code patterns and runtime coverage metrics.

add this pass to your rust projects to enhance code safety and gain a deeper understanding of unsafe code usage.
