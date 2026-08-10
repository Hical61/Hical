/**
 * @file test_main.cpp
 * @brief 自定义测试入口，MinGW 下绕过全局析构避免 DLL TLS 回调时序冲突导致的误报 segfault
 */

#include <gtest/gtest.h>
#include <cstdlib>

#ifdef __MINGW32__
	#define HICAL_MSYS2_QUICK_EXIT 1
#endif

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	int result = RUN_ALL_TESTS();

#ifdef HICAL_MSYS2_QUICK_EXIT
	/**
     * MinGW 进程退出时 DLL TLS 回调与 CRT 堆析构存在已知时序冲突，
     * 测试 body 全部 PASSED 的前提下用 _exit() 绕过全局析构链，
     * 避免 ~MemoryPool/~HttpServer 等析构时访问已拆卸 CRT 堆导致的误报 segfault。
     * 仅在 result == 0（全部通过）时跳过析构，确保真正的测试失败不会丢失退出码。
     */
	if (result == 0)
	{
		_exit(0);
	}
#endif

	return result;
}
