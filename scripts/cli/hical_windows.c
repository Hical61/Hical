/**
 * @file hical_windows.c
 * @brief Windows 下启动 hical.py，原样传递 UTF-16 参数。
 */

#include <windows.h>

// CreateProcessW 的命令行上限包含结尾空字符。
enum
{
	kBufferCapacity = 32767,
	kErrorBufferCapacity = 1024,
	kLauncherError = 1
};

typedef struct CommandLine
{
	WCHAR text[kBufferCapacity];
	DWORD length;
} CommandLine;

/**
 * @brief 把错误写到 stderr，重定向时用 UTF-8。
 * @param message 以空字符结尾的错误说明
 */
static void writeError(const WCHAR* message)
{
	HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
	DWORD written;
	DWORD mode;
	int length;
	char utf8[kErrorBufferCapacity];
	if (GetConsoleMode(output, &mode))
	{
		WriteConsoleW(output, message, (DWORD)lstrlenW(message), &written, NULL);
		return;
	}
	length = WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8, sizeof(utf8), NULL, NULL);
	if (length > 0)
	{
		WriteFile(output, utf8, (DWORD)(length - 1), &written, NULL);
	}
}

/**
 * @brief 根据 hical.exe 的位置找安装好的 hical.py。
 * @param path 容量为 kBufferCapacity 的输出缓冲
 * @return 脚本存在且路径未截断时返回 TRUE
 */
static BOOL findScript(WCHAR* path)
{
	static const WCHAR kRelativePath[] = L"share\\hical\\cli\\hical.py";
	static const DWORD kParentComponents = 2;
	DWORD length = GetModuleFileNameW(NULL, path, kBufferCapacity);
	DWORD attributes;
	DWORD component;
	if (length == 0 || length >= kBufferCapacity)
	{
		return FALSE;
	}
	// 带 \\?\ 前缀时，Windows 不会处理路径里的 ..，所以直接去掉末尾的 bin\hical.exe。
	for (component = 0; component < kParentComponents; ++component)
	{
		if (component > 0)
		{
			--length;
		}
		while (length > 0 && path[length - 1] != L'\\' && path[length - 1] != L'/')
		{
			--length;
		}
		if (length == 0)
		{
			return FALSE;
		}
	}
	if (length + ARRAYSIZE(kRelativePath) > kBufferCapacity)
	{
		return FALSE;
	}
	CopyMemory(path + length, kRelativePath, sizeof(kRelativePath));
	attributes = GetFileAttributesW(path);
	return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/**
 * @brief 在 PATH 里找 python.exe。
 * @param path 容量为 kBufferCapacity 的输出缓冲
 * @return 找到未截断的可执行路径时返回 TRUE
 */
static BOOL findPython(WCHAR* path)
{
	WCHAR searchPath[kBufferCapacity];
	DWORD length = GetEnvironmentVariableW(L"PATH", searchPath, kBufferCapacity);
	DWORD source = 0;
	DWORD target = 0;
	if (length == 0 || length >= kBufferCapacity)
	{
		return FALSE;
	}
	// SearchPathW 不会像 cmd 那样去掉 PATH 各项外层的双引号。
	while (searchPath[source])
	{
		DWORD start = source;
		DWORD end;
		while (searchPath[source] && searchPath[source] != L';')
		{
			++source;
		}
		end = source;
		if (end - start >= 2 && searchPath[start] == L'"' && searchPath[end - 1] == L'"')
		{
			++start;
			--end;
		}
		while (start < end)
		{
			searchPath[target++] = searchPath[start++];
		}
		if (searchPath[source] == L';')
		{
			searchPath[target++] = searchPath[source++];
		}
	}
	searchPath[target] = L'\0';
	if (target == 0)
	{
		return FALSE;
	}
	length = SearchPathW(searchPath, L"python.exe", NULL, kBufferCapacity, path, NULL);
	return length > 0 && length < kBufferCapacity;
}

/**
 * @brief 重复追加字符，给结尾的空字符留一个位置。
 * @param command 正在组装的命令行
 * @param value 要追加的字符
 * @param count 重复次数
 * @return 未超过 Windows 命令行上限时返回 TRUE
 */
static BOOL appendRepeated(CommandLine* command, WCHAR value, DWORD count)
{
	if (count >= kBufferCapacity - command->length)
	{
		return FALSE;
	}
	while (count > 0)
	{
		command->text[command->length++] = value;
		--count;
	}
	command->text[command->length] = L'\0';
	return TRUE;
}

/**
 * @brief 按 Windows CRT 的规则给单个参数加引号。
 * @param command 正在组装的命令行
 * @param argument 原始 UTF-16 参数，包括空字符串
 * @return 参数完整写入时返回 TRUE
 */
static BOOL appendArgument(CommandLine* command, const WCHAR* argument)
{
	DWORD slashes = 0;
	if ((command->length > 0 && !appendRepeated(command, L' ', 1)) || !appendRepeated(command, L'"', 1))
	{
		return FALSE;
	}
	while (*argument)
	{
		if (*argument == L'\\')
		{
			++slashes;
		}
		else
		{
			// 引号前的反斜杠翻倍，再加一个反斜杠转义引号本身。
			DWORD count = *argument == L'"' ? slashes * 2 + 1 : slashes;
			if (!appendRepeated(command, L'\\', count) || !appendRepeated(command, *argument, 1))
			{
				return FALSE;
			}
			slashes = 0;
		}
		++argument;
	}
	// 末尾反斜杠翻倍，避免吃掉包围参数的右引号。
	return appendRepeated(command, L'\\', slashes * 2) && appendRepeated(command, L'"', 1);
}

/**
 * @brief 启动 Python，让它沿用当前进程的标准输入、输出和错误流。
 * @param python Python 可执行文件的完整路径
 * @param command 可被 CreateProcessW 修改的命令行
 * @return Python 的退出码。启动或等待失败返回 1
 */
static int runPython(const WCHAR* python, WCHAR* command)
{
	STARTUPINFOW startupInfo = {0};
	PROCESS_INFORMATION processInfo = {0};
	DWORD exitCode;
	BOOL completed;
	startupInfo.cb = sizeof(startupInfo);
	startupInfo.dwFlags = STARTF_USESTDHANDLES;
	startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	startupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	startupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	if (!CreateProcessW(python, command, NULL, NULL, TRUE, 0, NULL, NULL, &startupInfo, &processInfo))
	{
		writeError(L"错误：无法启动 Python，请检查 PATH 里的 python.exe。\n");
		return kLauncherError;
	}
	CloseHandle(processInfo.hThread);
	completed = WaitForSingleObject(processInfo.hProcess, INFINITE) == WAIT_OBJECT_0
				&& GetExitCodeProcess(processInfo.hProcess, &exitCode);
	CloseHandle(processInfo.hProcess);
	if (!completed)
	{
		writeError(L"错误：取不到 Python 的退出码。\n");
		return kLauncherError;
	}
	return (int)exitCode;
}

/**
 * @brief 把命令行参数传给安装目录里的 hical.py。
 * @param argc 参数数量，包含本程序名
 * @param argv Windows CRT 解析的 UTF-16 参数
 * @return 生成器的退出码。入口自身失败返回 1
 */
int wmain(int argc, WCHAR** argv)
{
	WCHAR script[kBufferCapacity];
	WCHAR python[kBufferCapacity];
	CommandLine command;
	int index;
	command.length = 0;
	command.text[0] = L'\0';
	if (!findScript(script))
	{
		writeError(L"错误：找不到安装目录里的 share/hical/cli/hical.py。\n");
		return kLauncherError;
	}
	if (!findPython(python))
	{
		writeError(L"错误：找不到 Python，请安装 Python 3.10 或更新版本，并把 python.exe 所在目录加到 PATH。\n");
		return kLauncherError;
	}
	if (!appendArgument(&command, python) || !appendArgument(&command, script))
	{
		writeError(L"错误：命令行超过 Windows 长度上限。\n");
		return kLauncherError;
	}
	for (index = 1; index < argc; ++index)
	{
		if (!appendArgument(&command, argv[index]))
		{
			writeError(L"错误：命令行超过 Windows 长度上限。\n");
			return kLauncherError;
		}
	}
	return runPython(python, command.text);
}
