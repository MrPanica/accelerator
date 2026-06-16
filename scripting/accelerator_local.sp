#include <sourcemod>
#include <accelerator>

#pragma semicolon 1
#pragma newdecls required

public Plugin myinfo =
{
	name = "Accelerator Local",
	author = "OpenAI",
	description = "Local dump console commands for Accelerator",
	version = "1.0.0",
	url = "https://github.com/asherkin/accelerator"
};

void ReplyMultiline(int client, const char[] prefix, const char[] text)
{
	ReplyToCommand(client, "%s", prefix);

	int length = strlen(text);
	int start = 0;

	while (start < length)
	{
		char chunk[900];
		int write = 0;

		while (start < length && write < sizeof(chunk) - 1)
		{
			char c = text[start++];
			if (c == '\n')
			{
				break;
			}

			chunk[write++] = c;
		}

		chunk[write] = '\0';

		if (chunk[0] != '\0')
		{
			ReplyToCommand(client, "%s", chunk);
		}
	}
}

public void OnPluginStart()
{
	RegAdminCmd("sm_dump_list", Command_DumpList, ADMFLAG_RCON, "Lists locally pending Accelerator dump files.");
	RegAdminCmd("sm_proc_dump", Command_ProcessDump, ADMFLAG_RCON, "Starts asynchronous processing of a local Accelerator dump.");
	RegAdminCmd("sm_proc_stack_dump", Command_ProcessStackDump, ADMFLAG_RCON, "Starts asynchronous construction of a local trace report.");
	RegAdminCmd("sm_proc_jobs", Command_ProcessJobs, ADMFLAG_RCON, "Lists local dump processing jobs.");
	RegAdminCmd("sm_proc_status", Command_ProcessStatus, ADMFLAG_RCON, "Shows the status of a local dump processing job.");
	RegAdminCmd("sm_proc_result", Command_ProcessResult, ADMFLAG_RCON, "Prints the final result of a completed local dump processing job.");
	RegAdminCmd("sm_dump_crash_test", Command_CrashTest, ADMFLAG_RCON, "Intentionally crashes the server so Accelerator can capture a test dump.");
}

Action Command_DumpList(int client, int args)
{
	char buffer[8192];
	if (!Accelerator_LocalListDumps(buffer, sizeof(buffer)))
	{
		ReplyToCommand(client, "[Accelerator] Failed to list dumps.");
		return Plugin_Handled;
	}

	ReplyToCommand(client, "[Accelerator] %s", buffer);
	return Plugin_Handled;
}

Action Command_ProcessDump(int client, int args)
{
	if (args < 2 || args > 3)
	{
		ReplyToCommand(client, "[Accelerator] Usage: sm_proc_dump <dump> <mode> [output-file]");
		return Plugin_Handled;
	}

	char dumpName[256];
	char mode[64];
	char requestedOutput[PLATFORM_MAX_PATH];
	char status[512];
	char consoleDump[32768];
	int jobId = 0;

	GetCmdArg(1, dumpName, sizeof(dumpName));
	GetCmdArg(2, mode, sizeof(mode));
	if (StrEqual(mode, "console", false))
	{
		if (args != 2)
		{
			ReplyToCommand(client, "[Accelerator] Usage: sm_proc_dump <dump> console");
			return Plugin_Handled;
		}

		if (!Accelerator_LocalGetConsoleDump(dumpName, consoleDump, sizeof(consoleDump)))
		{
			ReplyToCommand(client, "[Accelerator] Failed to read dump console history.");
			return Plugin_Handled;
		}

		Format(status, sizeof(status), "[Accelerator] Console history for %s:", dumpName);
		ReplyMultiline(client, status, consoleDump);
		return Plugin_Handled;
	}

	if (args >= 3)
	{
		GetCmdArg(3, requestedOutput, sizeof(requestedOutput));
	}
	else
	{
		requestedOutput[0] = '\0';
	}

	if (!Accelerator_LocalStartProcessDump(dumpName, mode, requestedOutput, jobId, status, sizeof(status)))
	{
		ReplyToCommand(client, "[Accelerator] Failed to start dump processing job.");
		return Plugin_Handled;
	}

	ReplyToCommand(client, "[Accelerator] %s", status);
	ReplyToCommand(client, "[Accelerator] Job #%d was queued. The final result will be printed automatically.", jobId);
	return Plugin_Handled;
}

Action Command_ProcessStackDump(int client, int args)
{
	if (args != 1)
	{
		ReplyToCommand(client, "[Accelerator] Usage: sm_proc_stack_dump <dump>");
		return Plugin_Handled;
	}

	char dumpName[256];
	char status[512];
	int jobId = 0;
	GetCmdArg(1, dumpName, sizeof(dumpName));

	if (!Accelerator_LocalStartStackDump(dumpName, jobId, status, sizeof(status)))
	{
		ReplyToCommand(client, "[Accelerator] Failed to start stack dump job.");
		return Plugin_Handled;
	}

	ReplyToCommand(client, "[Accelerator] %s", status);
	ReplyToCommand(client, "[Accelerator] Job #%d was queued. The final result will be printed automatically.", jobId);
	return Plugin_Handled;
}

Action Command_ProcessJobs(int client, int args)
{
	char buffer[8192];
	if (!Accelerator_LocalListJobs(buffer, sizeof(buffer)))
	{
		ReplyToCommand(client, "[Accelerator] Failed to list local dump jobs.");
		return Plugin_Handled;
	}

	ReplyToCommand(client, "[Accelerator] %s", buffer);
	return Plugin_Handled;
}

Action Command_ProcessStatus(int client, int args)
{
	if (args != 1)
	{
		ReplyToCommand(client, "[Accelerator] Usage: sm_proc_status <job-id>");
		return Plugin_Handled;
	}

	char arg[32];
	char state[32];
	char status[512];
	char outputPath[PLATFORM_MAX_PATH];
	GetCmdArg(1, arg, sizeof(arg));
	int jobId = StringToInt(arg);

	if (!Accelerator_LocalGetJobStatus(jobId, state, sizeof(state), status, sizeof(status), outputPath, sizeof(outputPath)))
	{
		ReplyToCommand(client, "[Accelerator] Failed to read job status.");
		return Plugin_Handled;
	}

	if (outputPath[0] != '\0')
	{
		ReplyToCommand(client, "[Accelerator] Job #%d [%s]: %s | output=%s", jobId, state, status, outputPath);
	}
	else
	{
		ReplyToCommand(client, "[Accelerator] Job #%d [%s]: %s", jobId, state, status);
	}
	return Plugin_Handled;
}

Action Command_ProcessResult(int client, int args)
{
	if (args != 1)
	{
		ReplyToCommand(client, "[Accelerator] Usage: sm_proc_result <job-id>");
		return Plugin_Handled;
	}

	char arg[32];
	char state[32];
	char status[512];
	char outputPath[PLATFORM_MAX_PATH];
	char result[32768];
	GetCmdArg(1, arg, sizeof(arg));
	int jobId = StringToInt(arg);

	if (!Accelerator_LocalGetJobStatus(jobId, state, sizeof(state), status, sizeof(status), outputPath, sizeof(outputPath)))
	{
		ReplyToCommand(client, "[Accelerator] Failed to read job status.");
		return Plugin_Handled;
	}

	if (!StrEqual(state, "done", false))
	{
		if (outputPath[0] != '\0')
		{
			ReplyToCommand(client, "[Accelerator] Job #%d [%s]: %s | output=%s", jobId, state, status, outputPath);
		}
		else
		{
			ReplyToCommand(client, "[Accelerator] Job #%d [%s]: %s", jobId, state, status);
		}

		return Plugin_Handled;
	}

	if (!Accelerator_LocalGetJobResult(jobId, result, sizeof(result)))
	{
		ReplyToCommand(client, "[Accelerator] Failed to read job result.");
		return Plugin_Handled;
	}

	ReplyMultiline(client, "[Accelerator] Job result:", result);
	return Plugin_Handled;
}

Action Command_CrashTest(int client, int args)
{
	ReplyToCommand(client, "[Accelerator] Crash test requested. The server will crash intentionally now.");
	Accelerator_LocalCrashTest();
	return Plugin_Handled;
}
