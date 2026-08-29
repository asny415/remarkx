#pragma once

// 轻量崩溃上下文：记录当前正在执行的操作，崩溃处理器写入 crash.log。
// 在关键函数入口调用 remarkxSetCtx("...")，崩溃时即可知道发生在哪个环节。
void remarkxSetCtx(const char *s);
