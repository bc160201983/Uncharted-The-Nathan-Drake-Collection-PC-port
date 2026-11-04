# Native PS4 U3 blocking resource loader

Static addresses below refer to the supplied U3 memory image, SHA-256 E68BA596DEC2439CB93738CB2BF4057A6D2A080705998C8AD5BF024391BE7B51, before runtime relocation. New analysis used the copied Ghidra 12.2 project in read-only mode.

The exact counterpart of PS3 blocking module loader `0x009afb74` is PS4 `0x011f85e0`.

```c
uint64_t load_module_blocking(uint32_t module_hash, const uint32_t *allocation_tag);
```

Arguments use the game's System V x86-64 ABI: EDI contains the module hash, RSI points to an initialized 32-bit tag. Return AL is zero on rejected enqueue and one after the wait loop. Native callers use tag `0x12` for ordinary script modules. The same wrapper also has a native string convenience function at `0x012c0310`: it hashes its RDI string, supplies tag `0x12`, then calls the blocking wrapper.

For U3 `net`, the observed CRC32 module hash is `0x4240ef2e`. This is the native PS4 package that defines the missing `*custom-characters*` symbol `0xbd3bb3b9`.

## Verification

- Wrapper `0x011f85e0` calls `AddModuleRequest` at `0x011fcd70` with the requested hash, copied tag, and third argument 1.
- `AddModuleRequest` has an explicit diagnostic naming it and recursively acquires dependency modules. It increments the reference count if a request for the same module is already active.
- Wrapper pumps `0x011f7f40(1)` until the module hash exists in the module map and entry +`0x18` is nonzero, or global shutdown byte `0x01b24c39` becomes nonzero.
- Native caller `0x012bdef0` loads `bp-ucctweaks-script` through this wrapper with tag `0x12`, providing an independent argument/usage control.
- `ps4-blocking-loader-assembly.txt` independently verifies EDI/RSI and the exact `CALL` instructions. The 16-byte wrapper prologue is `554889e54157415641554154534883ec18`.
- `ps4-blocking-loader.c` retains fresh decompilation of the wrapper, dependency enqueue, and string convenience function.

## Bootstrap requirements and limitations

The module metadata table and dependencies must already be registered. `AddModuleRequest` asserts on an unknown module rather than discovering metadata from a file path. The normal initialized main-menu stage is therefore a more justified hook point than very early process startup.

Repeated calls can increment module reference counts. A bootstrap should load once or skip loading if the required symbol is already present, and should confirm the symbol after the wait. The wrapper can return one during shutdown without proving a module became available. A hook must preserve the original guest ABI, stack alignment, register state, and overwritten instructions.

No runtime resource registration or restoration success was observed in this analysis. The loader identification is supported by static code and native callers; the proposed use for loading `net` remains a runtime experiment.
