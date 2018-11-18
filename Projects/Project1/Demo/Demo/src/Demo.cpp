#include "platform\windows.hpp"
#include "type\types.hpp"

#include <iostream>

int WINAPI WinMain(_In_ HINSTANCE instance,
				   _In_opt_ [[maybe_unused]] HINSTANCE prev_instance,
				   _In_     [[maybe_unused]] LPSTR lpCmdLine,
				   _In_ int nCmdShow) {

	
	#ifdef _DEBUG
	const int debug_flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
	// Perform automatic leak checking at program exit through a call to
	// _CrtDumpMemoryLeaks and generate an error report if the application
	// failed to free all the memory it allocated.
	_CrtSetDbgFlag(debug_flags | _CRTDBG_LEAK_CHECK_DF);
	#endif

	//AddUnhandledExceptionFilter();

	// Initialize a console.
	//InitializeConsole();
	//PrintConsoleHeader();
	
	(void)instance;
	(void)nCmdShow;

	std::cout << "Hello World!\n";

	return 0;
}
