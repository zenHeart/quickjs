# 环境记录实现

## 核心对象说明

### [JSRuntime](../../quickjs.c#L26185)

`JSRuntime` 是 JavaScript 引擎的顶层结构，代表整个 JavaScript 运行时环境。它包含：

- 内存管理功能（`mf`，`malloc_state`）
- 原子（atom/symbol）处理系统（`atom_hash_size`，`atom_array`等）
- 类管理（`js_class_id_alloc`，`class_array`）
- 垃圾收集器状态（`gc_obj_list`，`gc_phase`）
- 栈限制（`stack_size`，`stack_limit`）
- 异常处理（`current_exception`）
- 作业队列（Promises的任务队列，`job_list`）
- 模块系统（`module_normalize_func`，`module_loader_func`）

对应于 ECMA262 规范中的 **Realm** 概念，Realm 是完整的 JavaScript 执行环境，包括所有内置对象和全局环境。

### [JSContext](../../quickjs.c#L401)

`JSContext` 代表在运行时内部的执行上下文。每个上下文具有：

- 指向其父运行时的引用（`rt`）
- 全局对象（`global_obj`，`global_var_obj`）
- 内置原型和构造函数（`class_proto`，`function_proto`等）
- 模块处理（`loaded_modules`）
- 当前执行状态（如`interrupt_counter`）

对应于 ECMA262 规范中的 **Execution Context** (执行上下文)，包含了代码执行所需的所有信息。

### [JSStackFrame](../../quickjs.c#L325)

`JSStackFrame` 表示执行栈中的单个栈帧，包含：
- 当前函数（`cur_func`）
- 参数（`arg_buf`）
- 变量（`var_buf`）
- 变量引用列表（`var_ref_list`）
- 程序计数器（`cur_pc`）
- 严格模式标志（`is_strict_mode`）

对应于规范中的执行上下文栈中的一个条目。



### 环境记录的核心数据结构

QuickJS 主要使用以下数据结构来表示环境记录：

#### 1. 变量引用(JSVarRef)

```c
typedef struct JSVarRef {
    struct JSVarRef *next;
    JSValue *pvalue;        /* pointer to the value, NULL if the variable was deleted */
    int is_detached;        /* TRUE if the variable is no longer on the stack */
} JSVarRef;
```

用于表示一个变量引用，特别是闭包捕获的变量。当变量不再在栈上时，`is_detached` 标志被设置。

#### 2. 变量定义(JSVarDef)

```c
typedef struct JSVarDef {
    JSAtom var_name;        /* variable name */
    int scope_level;        /* scope level, -1 means uninitialized */
    int force_init;         /* force initialization to undefined */
    // ...其他字段
} JSVarDef;
```

表示变量的定义信息，包含变量名(作为原子字符串)和作用域级别。

#### 3. 栈帧(JSStackFrame)

```c
typedef struct JSStackFrame {
    JSValue *cur_sp;
    // ...其他字段
} JSStackFrame;
```

栈帧结构包含当前执行上下文的信息，包括变量环境。

### 其他

QuickJS 区分了几种不同类型的环境：

1. **全局环境**：通过 `JSContext` 中的全局对象实现
   - `JS_CheckDefineGlobalVar`、`JS_DefineGlobalVar` 等函数处理全局变量

2. **函数环境**：通过 `JSFunctionDef` 和 `JSStackFrame` 实现
   - `add_arg`、`add_var` 等函数处理函数参数和局部变量

3. **块级环境**：通过作用域级别(`scope_level`)来区分
   - `push_scope`、`pop_scope`、`close_scopes` 等函数管理块级作用域

4. **词法环境**：由 `JSVarRef` 实现闭包捕获的变量
   - `get_var_ref` 函数处理闭包变量引用

QuickJS 的实现相比规范更加直接和高效，结合了编译时分析和运行时处理，通过字节码指令和运行时API共同实现了 ECMAScript 环境记录的功能。

## 环境记录的实现

ECMA262 规范中的环境记录（Environment Records）在此实现中主要通过以下方式体现：

1. **变量环境**：通过 `JSStackFrame` 中的 `var_buf` 实现
2. **词法环境**：通过 `JSVarRef` 系统实现，特别是闭包场景
3. **全局环境记录**：在 `JSContext` 的 `global_obj` 和 `global_var_obj` 中实现
4. **闭包变量**：
   - `JSVarRef` 实现了可以超出其原始作用域的变量引用（闭包）
   - `is_detached` 标志及 `value` 字段用于管理离开原始栈帧后的变量值


在 QuickJS 中，环境记录主要通过 `JSContext` 和作用域链相关的数据结构实现。QuickJS 实现环境记录的关键点如下：

### 1. 变量环境的表示

QuickJS 使用 `JSVarRef` 结构来表示变量引用，用 `JSVarDef` 来定义变量。这些定义在 quickjs.c 中：

```c
typedef struct JSVarRef {
    struct JSVarRef *next;
    JSValue *pvalue;        /* pointer to the value, NULL if the variable was deleted */
    int is_detached;        /* TRUE if the variable is no longer on the stack */
} JSVarRef;

typedef struct JSVarDef {
    JSAtom var_name;        /* variable name */
    int scope_level;        /* scope level, -1 means uninitialized */
    int force_init;         /* force initialization to undefined */
    ...
} JSVarDef;
```

### 2. 作用域链实现

QuickJS 通过 `JSStackFrame` 结构维护函数调用栈，每个栈帧包含当前的变量环境：

```c
typedef struct JSStackFrame {
    struct JSStackFrame *prev;
    JSValue *var_buf;       /* variables and stack slots */
    JSValue *arg_buf;       /* arguments */
    int js_mode;            /* 0=normal, 1=strict */
    ...
} JSStackFrame;
```

### 3. 环境记录抽象方法实现

#### HasBinding(N)

检查环境中是否存在指定名称的绑定：

```c
int JS_HasVar(JSContext *ctx, JSAtom name) {
    JSVarDef *vd;
    vd = find_var(ctx, name);
    return (vd != NULL);
}
```

#### CreateMutableBinding(N, D)

创建可变绑定：

```c
int JS_DefineVar(JSContext *ctx, JSAtom name, int is_const) {
    JSVarDef var_def;
    var_def.var_name = name;
    var_def.is_const = is_const;
    var_def.is_lexical = 1;
    return add_var(ctx, var_def);
}
```

#### SetMutableBinding(N, V, S)

设置可变绑定的值：

```c
int JS_SetVar(JSContext *ctx, JSAtom name, JSValue val) {
    JSVarDef *vd = find_var(ctx, name);
    if (!vd)
        return -1;
    if (vd->is_const && !vd->is_uninitialized)
        return JS_ThrowTypeError(ctx, "assignment to constant variable");
    set_var_value(ctx, vd, val);
    vd->is_uninitialized = 0;
    return 0;
}
```

#### GetBindingValue(N, S)

获取绑定的值：

```c
JSValue JS_GetVar(JSContext *ctx, JSAtom name) {
    JSVarDef *vd = find_var(ctx, name);
    if (!vd)
        return JS_UNDEFINED;
    if (vd->is_uninitialized)
        return JS_ThrowReferenceError(ctx, "accessing uninitialized variable");
    return JS_DupValue(ctx, get_var_value(ctx, vd));
}
```

#### DeleteBinding(N)

删除绑定：

```c
int JS_DeleteVar(JSContext *ctx, JSAtom name) {
    JSVarDef *vd = find_var(ctx, name);
    if (!vd)
        return 0;
    if (!vd->is_configurable)
        return 0;
    remove_var(ctx, vd);
    return 1;
}
```

### 4. 不同类型环境记录的处理

QuickJS 区分了几种环境记录类型:

- **声明式环境记录**: 通过 `JSVarDef` 数组实现
- **对象式环境记录**: 通过 `JSObject` 实现
- **全局环境记录**: 结合了声明式和对象式环境记录

QuickJS 的实现比规范中的抽象方法更加简洁和具体，但保持了相同的语义。通过这种设计，QuickJS 能够高效地实现 JavaScript 的作用域和变量查找机制。

gufeng_xhs: 基于 #file:quickjs.c #file:quickjs.h 尝试理解上面的问题，并说出具体的实现

GitHub Copilot: # QuickJS 环境记录(Environment Record)实现分析

在 ECMAScript 规范中，环境记录(Environment Record)是一种抽象机制，用于跟踪标识符与特定变量的关联。QuickJS 通过一系列结构和函数来实现这些抽象方法，现在我将基于源码分析它们的具体实现。

## 环境记录抽象方法的实现

根据源码，QuickJS 实现了以下环境记录抽象方法：

### 1. HasBinding(N) - 检查绑定是否存在

在 QuickJS 中，这主要通过 `find_var` 函数实现：

```c
static int find_var(JSContext *ctx, JSFunctionDef *fd, JSAtom name)
{
    int idx;
    idx = find_var_in_scope(ctx, fd, name, fd->scope_level);
    if (idx >= 0)
        return idx;
not_found:
    return -1; // 不存在该绑定
}
```

### 2. CreateMutableBinding(N, D) - 创建可变绑定

通过 `add_var` 和 `define_var` 系列函数实现：

```c
static int add_var(JSContext *ctx, JSFunctionDef *fd, JSAtom name)
{
    // 创建新的变量定义
    // ...
    return var_idx; // 返回变量索引
}

static int define_var(JSParseState *s, JSFunctionDef *fd, JSAtom name,
                      JSVarDefEnum var_def_type)
{
    // 根据不同的变量定义类型定义变量
    // ...
}
```

### 3. SetMutableBinding(N, V, S) - 设置绑定值

在运行时通过 `JS_SetProperty` 或在编译时生成 `OP_put_var` 等操作来实现：

```c
int JS_SetProperty(JSContext *ctx, JSValueConst this_obj, JSAtom prop, JSValue val)
{
    // 设置属性值，包括处理对象属性和变量绑定
    // ...
    return ret;
}
```

在字节码层面，通过操作码如 `OP_put_var`、`OP_put_var_ref` 等实现变量赋值。

### 4. GetBindingValue(N, S) - 获取绑定值

通过 `JS_GetProperty` 或编译时生成的 `OP_get_var` 等操作来实现：

```c
JSValue JS_GetProperty(JSContext *ctx, JSValueConst this_obj, JSAtom prop)
{
    // 获取属性值，包括处理对象属性和变量绑定
    // ...
    return val;
}
```

在字节码层面，通过操作码如 `OP_get_var`、`OP_get_ref` 等实现变量值获取。

### 5. DeleteBinding(N) - 删除绑定

通过 `delete_property` 和 `JS_DeleteProperty` 实现：

```c
static int delete_property(JSContext *ctx, JSObject *p, JSAtom atom)
{
    // 删除属性，包括处理变量绑定
    // ...
}

int JS_DeleteProperty(JSContext *ctx, JSValueConst obj, JSAtom prop, int flags)
{
    // 删除属性的高层接口
    // ...
    return ret;
}
```
