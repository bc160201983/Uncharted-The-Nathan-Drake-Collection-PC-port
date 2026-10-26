# Runtime address-reservation diagnosis and per-child launch fix

The installed U3 PC runtime originally stopped before guest execution while reserving its PS4 address range `[0x1000000000, 0xfc00000000)`, with Windows error 487. Suspended child memory maps showed the child's initial private allocations/stack inside that range. This was independent of the multiplayer byte patch.

Nine suspended-process measurements tested only per-child creation attributes:

| Creation mitigation attribute | Conflicting layouts | Free layouts |
|---|---:|---:|
| Default (`0`) | 3 | 0 |
| Bottom-up ASLR off (`0x20000`) | 3 | 0 |
| Bottom-up and high-entropy ASLR off (`0x220000`) | 0 | 3 |

Every measurement child remained suspended and was terminated after its address map was inspected. No game code ran in those measurements.

`u3_child_process.py` applies the successful attribute through `STARTUPINFOEX`/`PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY` only to the exact configured Uncharted runtime. It inherits only redirected log output/error and NUL input, supplies a sorted Unicode environment, checks the required address range while the child is suspended, and then resumes it. Cleanup closes the attribute list, duplicated handles and thread handle; the process handle closes after exit.

A bounded `--help` smoke launch passed the address-range check, resumed, and reached the runtime's ordinary argument error `ERROR: cannot open boot file; run prepare.py first` in 0.059 seconds. Exit code 1 was expected for this command, which deliberately provided no prepared game image. This confirms progress beyond the old reservation failure, not successful game initialization.

No executable header, global registry setting, system mitigation policy, or installed launcher setting was changed. The ASLR override applies only to the spawned test child, so the helper is restricted to the specified runtime.

Evidence:

- `evidence/runtime-child-mitigation-layout.json`: all nine suspended-process measurements.
- `evidence/runtime-child-layout-help.json` and `.log`: resumed smoke result.
- `u3_child_process.py`: reusable launch helper.

Microsoft documents the child-process mitigation attribute and the two relevant flags in [UpdateProcThreadAttribute](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute).
