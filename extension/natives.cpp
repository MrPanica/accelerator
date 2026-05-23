#include <algorithm>
#include <amtl/am-string.h>
#include "extension.h"
#include "natives.h"

static cell_t Native_GetUploadedCrashCount(IPluginContext* context, const cell_t* params)
{
	return g_accelerator.GetUploadedCrashCount();
}

static cell_t Native_IsDoneUploadingCrashes(IPluginContext* context, const cell_t* params)
{
	if (g_accelerator.IsDoneUploading()) {
		return 1;
	}

	return 0;
}

static cell_t Native_GetCrashHTTPResponse(IPluginContext* context, const cell_t* params)
{
	if (!g_accelerator.IsDoneUploading()) {
		context->ReportError("Wait until accelerator is done uploading crashes before accessing crash information!");
		return 0;
	}

	int element = static_cast<int>(params[1]);
	const UploadedCrash* crash = g_accelerator.GetUploadedCrash(element);

	if (!crash) {
		context->ReportError("Crash index %i is invalid!", element);
		return 0;
	}

	char* buffer;
	context->LocalToString(params[2], &buffer);

	size_t maxsize = static_cast<size_t>(params[3]);

	ke::SafeStrcpy(buffer, maxsize, crash->GetHTTPResponse().c_str());

	return 0;
}

static cell_t Native_LocalListDumps(IPluginContext* context, const cell_t* params)
{
	char *buffer = nullptr;
	context->LocalToString(params[1], &buffer);
	size_t maxsize = static_cast<size_t>(params[2]);

	std::string output;
	std::string error;
	if (!Accelerator_LocalListDumps(output, error)) {
		context->ReportError("%s", error.c_str());
		return 0;
	}

	ke::SafeStrcpy(buffer, maxsize, output.c_str());
	return 1;
}

static cell_t Native_LocalProcessDump(IPluginContext* context, const cell_t* params)
{
	char *dumpName = nullptr;
	char *mode = nullptr;
	char *requestedOutput = nullptr;
	char *outputPath = nullptr;
	char *status = nullptr;

	context->LocalToString(params[1], &dumpName);
	context->LocalToString(params[2], &mode);
	context->LocalToString(params[3], &requestedOutput);
	context->LocalToString(params[4], &outputPath);
	context->LocalToString(params[6], &status);

	size_t outputPathLen = static_cast<size_t>(params[5]);
	size_t statusLen = static_cast<size_t>(params[7]);

	std::string resolvedOutputPath;
	std::string resolvedStatus;
	std::string error;
	if (!Accelerator_LocalProcessDump(dumpName, mode, requestedOutput, resolvedOutputPath, resolvedStatus, error)) {
		context->ReportError("%s", error.c_str());
		return 0;
	}

	ke::SafeStrcpy(outputPath, outputPathLen, resolvedOutputPath.c_str());
	ke::SafeStrcpy(status, statusLen, resolvedStatus.c_str());
	return 1;
}

static cell_t Native_LocalGetStackDump(IPluginContext* context, const cell_t* params)
{
	char *dumpName = nullptr;
	char *buffer = nullptr;
	context->LocalToString(params[1], &dumpName);
	context->LocalToString(params[2], &buffer);
	size_t maxsize = static_cast<size_t>(params[3]);

	std::string stackTrace;
	std::string error;
	if (!Accelerator_LocalGetStackDump(dumpName, stackTrace, error)) {
		context->ReportError("%s", error.c_str());
		return 0;
	}

	ke::SafeStrcpy(buffer, maxsize, stackTrace.c_str());
	return 1;
}

static cell_t Native_LocalGetConsoleDump(IPluginContext* context, const cell_t* params)
{
	char *dumpName = nullptr;
	char *buffer = nullptr;
	context->LocalToString(params[1], &dumpName);
	context->LocalToString(params[2], &buffer);
	size_t maxsize = static_cast<size_t>(params[3]);

	std::string consoleDump;
	std::string error;
	if (!Accelerator_LocalGetConsoleDump(dumpName, consoleDump, error)) {
		context->ReportError("%s", error.c_str());
		return 0;
	}

	ke::SafeStrcpy(buffer, maxsize, consoleDump.c_str());
	return 1;
}

static cell_t Native_LocalCrashTest(IPluginContext* context, const cell_t* params)
{
	Accelerator_LocalTriggerCrashTest();
	return 0;
}

void natives::Setup(std::vector<sp_nativeinfo_t>& vec)
{
	sp_nativeinfo_t list[] = {
		{"Accelerator_GetUploadedCrashCount", Native_GetUploadedCrashCount},
		{"Accelerator_IsDoneUploadingCrashes", Native_IsDoneUploadingCrashes},
		{"Accelerator_GetCrashHTTPResponse", Native_GetCrashHTTPResponse},
		{"Accelerator_LocalListDumps", Native_LocalListDumps},
		{"Accelerator_LocalProcessDump", Native_LocalProcessDump},
		{"Accelerator_LocalGetStackDump", Native_LocalGetStackDump},
		{"Accelerator_LocalGetConsoleDump", Native_LocalGetConsoleDump},
		{"Accelerator_LocalCrashTest", Native_LocalCrashTest},
	};

	vec.insert(vec.end(), std::begin(list), std::end(list));
}
