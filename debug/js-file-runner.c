#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-libc.h" // 添加这个头文件以支持标准库

// 读取文件内容
static char *read_file(const char *filename, size_t *size_ptr) {
    FILE *f;
    size_t size;
    char *buf;
    
    f = fopen(filename, "rb");
    if (!f) {
        perror(filename);
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    
    if (fread(buf, 1, size, f) != size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    
    buf[size] = '\0';
    fclose(f);
    
    if (size_ptr)
        *size_ptr = size;
    return buf;
}

// 打印JavaScript值
static void print_value(JSContext *ctx, JSValueConst val) {
    const char *str;
    
    str = JS_ToCString(ctx, val);
    if (str) {
        printf("%s\n", str);
        JS_FreeCString(ctx, str);
    } else {
        printf("[Exception]\n");
    }
}

int main(int argc, char **argv) {
    JSRuntime *rt;
    JSContext *ctx;
    char *buf;
    size_t buf_len;
    JSValue val;
    int ret = 1;
    
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <filename.js>\n", argv[0]);
        return 1;
    }
    
    // 读取JavaScript文件
    buf = read_file(argv[1], &buf_len);
    if (!buf) {
        fprintf(stderr, "Could not read file '%s'\n", argv[1]);
        return 1;
    }
    
    // 初始化QuickJS运行时和上下文
    rt = JS_NewRuntime();
    if (!rt) {
        fprintf(stderr, "Failed to create JS runtime\n");
        free(buf);
        return 1;
    }
    
    // 设置模块加载器
    js_std_set_worker_new_context_func(JS_NewContext);
    js_std_init_handlers(rt);
    
    ctx = JS_NewContext(rt);
    if (!ctx) {
        fprintf(stderr, "Failed to create JS context\n");
        JS_FreeRuntime(rt);
        free(buf);
        return 1;
    }
    
    // 初始化标准模块
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    
    // 设置模块加载器函数
    JS_SetModuleLoaderFunc(rt, NULL, js_module_loader, NULL);
    
    // 添加标准辅助函数（包括console对象）
    js_std_add_helpers(ctx, argc, argv);
    
    // 执行JavaScript代码
    val = JS_Eval(ctx, buf, buf_len, argv[1], JS_EVAL_TYPE_GLOBAL);
    
    if (JS_IsException(val)) {
        // 打印异常信息
        JSValue exception = JS_GetException(ctx);
        printf("Exception: ");
        print_value(ctx, exception);
        
        // 如果有堆栈信息，也打印出来
        JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
        if (!JS_IsUndefined(stack)) {
            printf("Stack trace:\n");
            print_value(ctx, stack);
        }
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, exception);
    } else {
        // 打印执行结果
        printf("Result: ");
        print_value(ctx, val);
        ret = 0;
    }
    
    JS_FreeValue(ctx, val);
    
    // 执行任何挂起的作业（处理Promise等异步操作）
    js_std_loop(ctx);
    
    // 释放资源
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    free(buf);
    
    return ret;
}