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
		strcopy(chunk, sizeof(chunk), text[start]);

		int newline = FindCharInString(chunk, '\n');
		if (newline != -1)
		{
			chunk[newline] = '\0';
		}

		if (chunk[0] != '\0')
		{
			ReplyToCommand(client, "%s", chunk);
		}

		if (newline == -1)
		{
			break;
		}

		start += newline + 1;
	}
}

public void OnPluginStart()
{
	RegAdminCmd("sm_dump_list", Command_DumpList, ADMFLAG_RCON, "Lists locally pending Accelerator dump files.");
	RegAdminCmd("sm_proc_dump", Command_ProcessDump, ADMFLAG_RCON, "Processes a local Accelerator dump and writes the result to a file.");
	RegAdminCmd("sm_proc_stack_dump", Command_ProcessStackDump, ADMFLAG_RCON, "Prints the full trace and metadata for a local Accelerator dump, without console history.");
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
	char outputPath[PLATFORM_MAX_PATH];
	char status[512];
	char consoleDump[32768];

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

	if (!Accelerator_LocalProcessDump(dumpName, mode, requestedOutput, outputPath, sizeof(outputPath), status, sizeof(status)))
	{
		ReplyToCommand(client, "[Accelerator] Failed to process dump.");
		return Plugin_Handled;
	}

	ReplyToCommand(client, "[Accelerator] %s", status);
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
	char stackTrace[32768];
	char status[512];
	GetCmdArg(1, dumpName, sizeof(dumpName));

	if (!Accelerator_LocalGetStackDump(dumpName, stackTrace, sizeof(stackTrace)))
	{
		ReplyToCommand(client, "[Accelerator] Failed to build stack trace.");
		return Plugin_Handled;
	}

	Format(status, sizeof(status), "[Accelerator] Stack trace for %s:", dumpName);
	ReplyMultiline(client, status, stackTrace);
	return Plugin_Handled;
}

Action Command_CrashTest(int client, int args)
{
	ReplyToCommand(client, "[Accelerator] Crash test requested. The server will crash intentionally now.");
	Accelerator_LocalCrashTest();
	return Plugin_Handled;
}
