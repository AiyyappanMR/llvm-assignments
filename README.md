# Assignment 1: Local Optimizations in LLVM

This folder contains a local (basic-block level) optimization pass for LLVM,
written inside LLVM's existing `HelloWorld` pass.

## Folder contents

| File | Description |
|---|---|
| `HelloWorld.cpp` | Source code of the optimization pass |
| `README.md` | This file |

Test cases are not included. Use your own `.c` test files with the commands in section 3.

---

## 1. What the pass does

The pass runs on every function. It applies the optimizations below to each
basic block and repeats them until nothing changes, because one optimization
often enables another (for example, folding `4 + a` lets the result be
propagated, which allows more folding).

At `-O0`, every C variable is an `alloca` that is read and written through
`load` and `store`. The pass only tracks variables whose address never escapes,
meaning the variable is used only by plain loads and stores. This keeps it safe
in the presence of pointers and function calls.

| # | Optimization | Function in code | Example |
|---|---|---|---|
| 1 | **Constant folding** | `nanFoldBinaryOp`, `nanConstFoldProp` | `4 + 2` becomes `6` |
| 2 | **Constant propagation** | `nanConstFoldProp` | `a = 2; x = a + 1` becomes `x = 2 + 1` |
| 3 | **Copy propagation** | `nanCopyProp` | `c = d; e = c + b` becomes `e = d + b` |
| 4 | **Algebraic identities** | `nanAlgebraicXopId` | `x+0`, `0+x`, `x-0`, `x*1`, `1*x`, `x/1` become `x` |
| 5 | **Strength reduction** | `nanReduceStrength` | `x * 8` becomes `x << 3`, `2 * x` becomes `x << 1`, unsigned `x / 4` becomes `x >> 2` |
| 6 | **Dead code elimination** | `nanIsDeadInst`, `nanDCE` | Removes instructions whose result is never used |
| 7 | **Redundant store elimination** | `nanDCE` | `a = 3; a = 3;` keeps only the first store |
| 8 | **Write-only variable removal** | `nanRemoveWriteOnlyVars` | Deletes variables that are written but never read, along with all their stores |

Signed division (`sdiv`) is not turned into a shift, because the results differ
for negative numbers: `-3 / 2 = -1`, but `-3 >> 1 = -2`.

The pass prints each change it makes (for example `Folding ...`,
`Propagating constant ...`, `Replacing ... with ...`, `DCE ...`) to the terminal,
followed by the final IR of each basic block.

---

## 2. Setting up the pass

The `HelloWorld` pass is already part of LLVM and registered as `helloworld`,
so only one file needs to be replaced.

1. Open `llvm/lib/Transforms/Utils/HelloWorld.cpp` in your `llvm-project` source tree.
2. Replace its entire contents with the contents of `HelloWorld.cpp` from this folder.
3. Rebuild `opt` from your build directory:

   ```bash
   ninja opt
   ```

   If you configured the build with Makefiles, use `make opt -j$(nproc)` instead.

No other files (header, `PassRegistry.def`, or `CMakeLists.txt`) need to change.

---

## 3. Running the tests

Run these commands from the folder containing your test file. Replace `test`
with the name of your test file. `clang` and `opt` are the binaries from your
LLVM build's `bin/` directory. Either add that directory to your `PATH`, or
prefix the commands with its full path.

### Step 1: Generate the IR before optimization

```bash
clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone -fno-discard-value-names test.c -o test.ll
```

What the flags do:

- `-O0` stops clang from running its own optimizations, so the output contains only the changes made by this pass.
- `-Xclang -disable-O0-optnone` is **required**. Without it, clang marks every function `optnone`, and `opt` then silently skips the pass, so the output would be the same as the input.
- `-fno-discard-value-names` keeps readable names such as `%a` and `%result` in the IR.

### Step 2: Run the pass to get the optimized IR

```bash
opt -S -passes=helloworld test.ll -o test_opt.ll
```

The log of applied optimizations is printed to the terminal. To save it to a
file, add `2> test.log` to the end of the command.

### Comparing the before and after IR

```bash
diff test.ll test_opt.ll
```

**Note:** if a computed value is never used (for example, `d = 2*c;` where `d`
is never read), the pass first optimizes it (the log shows `mul` replaced
with `shl`) and then removes it as dead code. In that case the change appears
only in the log, not in `test_opt.ll`.
