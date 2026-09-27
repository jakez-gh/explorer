#include "Explorer.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "FExplorerModule"

void FExplorerModule::StartupModule()
{
	// This code will execute after your module is loaded into memory;
	// the exact timing is specified in the .uplugin file.
}

void FExplorerModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.
	// For modules that bind Unreal delegates, it is important to unbind those
	// before we shut down, since they will no longer be callable after we are unloaded.
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FExplorerModule, Explorer)
