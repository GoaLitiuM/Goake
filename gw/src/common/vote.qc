const int VOTE_NOTHING = 0;
const int VOTE_IGNORED = 1; // special state used with ignoring spectator votes
const int VOTE_NO = 2;
const int VOTE_YES = 3;
const int VOTE_PICK = 4;
const int VOTE_BAN = 5;
const int CALLVOTE_INACTIVE = 0;
const int CALLVOTE_RESTARTING = 1;
const int CALLVOTE_RESTART = 2;
const int CALLVOTE_MODE = 3;
const int CALLVOTE_MAP = 4;
const int CALLVOTE_FRAGLIMIT = 5;
const int CALLVOTE_TIMELIMIT = 6;

void Vote_Init()
{
#ifdef CSQC
	registercommand("callvote");
	registercommand("vote");
#endif
}

#ifdef CSQC
void Vote_CallVote(string votecmd)
{
	int numargs = tokenize(votecmd);
	string arg1 = strtolower(argv(1));
	switch (strtolower(argv(0)))
	{
		case "mode":
		{
			if (strlen(arg1) > 0)
				sendevent("callvote", "eis", local_player, CALLVOTE_MODE, arg1);
			else
			{
				string supported_modes = "ffa, race"; // TODO: request supported modes from server
				println("Supported callvote modes: ", supported_modes);
			}
			break;
		}
		case "map":
		{
			if (strlen(arg1) > 0)
				sendevent("callvote", "eis", local_player, CALLVOTE_MAP, arg1);
			else
			{
				// TODO: request available maps from server
				println("Usage: callvote map <mapname>");
			}
			break;
		}
		case "fraglimit":
		{
			if (strlen(arg1) > 0)
			{
				int fraglimit = stoi(arg1);
				if (fraglimit < 0)
					fraglimit = 0;
				sendevent("callvote", "eii", local_player, CALLVOTE_FRAGLIMIT, fraglimit);
			}
			else
				println("Usage: callvote fraglimit <fraglimit>");
			break;
		}
		case "timelimit":
		{
			if (strlen(arg1) > 0)
			{
				float timelimit = stof(arg1);
				if (timelimit < 0)
					timelimit = 0;

				timelimit *= 60;
				sendevent("callvote", "eii", local_player, CALLVOTE_TIMELIMIT, (int)timelimit);
			}
			else
				println("Usage: callvote timelimit <timelimit in minutes>");
			break;
		}
		default:
			println("Supported callvote parameters: mode, map, fraglimit, timelimit");
			break;
	}
}

void Vote_Vote(string votecmd)
{
	int numargs = tokenize(votecmd);
	string vote = argv(0);
	string map;
	int prefix = str2chr(vote, 0);

	if (prefix == '+' || prefix == '-')
	{
		map = strtolower(substring(vote, 1, strlen(vote)-1));
		vote = (prefix == '+') ? "pick" : "ban";
	}
	else
		vote = strtolower(vote);

	if (!map)
		map = argv(1);

	if (vote == "yes")
		sendevent("vote", "ei", local_player, VOTE_YES);
	else if (vote == "no")
		sendevent("vote", "ei", local_player, VOTE_NO);
	else if (vote == "pick" && strlen(map) > 0)
		sendevent("vote", "eis", local_player, VOTE_PICK, map);
	else if ((vote == "ban" || vote == "drop") && strlen(map) > 0)
		sendevent("vote", "eis", local_player, VOTE_BAN, map);
	else
		println("Usage: vote yes|no|pick|ban <mapname>");
}

void Chat_SystemMessage(string msg);
void Vote_OnCallVote()
{
	string player_name = readString();
	int callvote = readByte();
	string value_str = readString();

	string callvote_str = "";
	switch (callvote)
	{
		case CALLVOTE_RESTART:
			callvote_str = "restart";
			break;
		case CALLVOTE_MODE:
			callvote_str = "mode";
			break;
		case CALLVOTE_MAP:
			callvote_str = "map";
			break;
		case CALLVOTE_FRAGLIMIT:
			callvote_str = "fraglimit";
			break;
		case CALLVOTE_TIMELIMIT:
			callvote_str = "timelimit";
			break;
		default:
			callvote_str = itos(callvote);
			break;
	}

	Chat_SystemMessage(strcat(player_name, " called a vote: ", callvote_str, " ", value_str));
}

void Vote_OnPlayerVote()
{
	string player_name = readString();
	int vote = readByte();

	string vote_str = "?";
	if (vote == VOTE_YES)
		vote_str = "yes";
	else if (vote == VOTE_NO)
		vote_str = "no";
	Chat_SystemMessage(strcat(player_name, " voted: ", vote_str));
}

void Vote_OnFinish()
{
	bool success = readByte();
	if (success)
		Chat_SystemMessage("Vote passed");
	else
		Chat_SystemMessage("Vote failed");
}
#endif

#ifdef SSQC
int callvote;
int callvote_timeout = 5;
float callvote_endtime;
bool callvote_instant;
variant callvote_value;

void Vote_ApplyChanges();
void GameMode_SetMode(string mode);
void GameMode_SetFragLimit(int fraglimit);
void GameMode_SetTimeLimit(int timelimit);

void Vote_Restart()
{
	callvote = CALLVOTE_RESTARTING;
	if (callvote_instant)
		Vote_ApplyChanges();
	else
		callvote_endtime = time + 2;
}

void Vote_Reset()
{
	callvote = CALLVOTE_INACTIVE;
	callvote_value = "";
}

void Vote_ApplyChanges()
{
	if (callvote == CALLVOTE_RESTARTING)
	{
		if (callvote_value != "") // deferred CALLVOTE_MAP
			changelevel((string)callvote_value);
		else //deferred CALLVOTE_RESTART
			changelevel(mapname);
		Vote_Reset();
	}
	else if (callvote == CALLVOTE_RESTART || callvote == CALLVOTE_MAP)
		Vote_Restart();
	else if (callvote == CALLVOTE_MODE)
	{
		GameMode_SetMode((string)callvote_value);
		Vote_Reset();
	}
	else if (callvote == CALLVOTE_FRAGLIMIT)
	{
		GameMode_SetFragLimit((int)callvote_value);
		Vote_Reset();
	}
	else if (callvote == CALLVOTE_TIMELIMIT)
	{
		GameMode_SetTimeLimit((int)callvote_value);
		Vote_Reset();
	}
}

void Vote_Passed()
{
	Vote_ApplyChanges();

	writeByte(MSG_ALL, SVC_CGAMEPACKET);
	writeByte(MSG_ALL, EVENT_CALLVOTE_FINISHED);
	writeByte(MSG_ALL, true);
}

void Vote_Failed()
{
	callvote = CALLVOTE_INACTIVE;

	writeByte(MSG_ALL, SVC_CGAMEPACKET);
	writeByte(MSG_ALL, EVENT_CALLVOTE_FINISHED);
	writeByte(MSG_ALL, false);
}

void Vote_Finish()
{
	int voted_yes = 0;
	int voted_no = 0;
	int players = 0;

	entity player = world;
	while (true)
	{
		player = find(player, classname, CLASS_PLAYER);
		if (player == world)
			break;

		if (strlen(infokey(player, "name")) == 0)
			continue;

		if (player.voted != VOTE_IGNORED && infokeyf(player, INFOKEY_P_SPECTATOR) == 0)
			players++;

		if (player.voted == VOTE_YES)
			voted_yes++;
		else if (player.voted == VOTE_NO)
			voted_no++;

		player.voted = VOTE_NOTHING;
	}

	callvote_instant = false;
	int vote_passlimit = players * 0.5;
	if (players <= 2)
	{
		vote_passlimit = players; // solo and duel players must all agree
		if (players <= 1)
			callvote_instant = true; // map changes are applied instantly for solos
	}

	if (voted_yes >= vote_passlimit)
		Vote_Passed();
	else
		Vote_Failed();
}

void Vote_Tick()
{
	if (callvote == CALLVOTE_INACTIVE)
		return;

	if (time >= callvote_endtime)
	{
		if (callvote == CALLVOTE_RESTARTING)
			Vote_ApplyChanges();
		else
			Vote_Finish();
	}
}

void Vote_OnPlayerVote(entity player, int vote, variant value)
{
	if (callvote == CALLVOTE_INACTIVE)
	{
		println("No votes are currently active");
		return;
	}

	if (player.voted != VOTE_NOTHING)
	{
		println("Player has already voted");
		return;
	}

	switch (callvote)
	{
		case CALLVOTE_RESTART:
		case CALLVOTE_MODE:
		case CALLVOTE_MAP:
		case CALLVOTE_FRAGLIMIT:
		case CALLVOTE_TIMELIMIT:
			if (vote != VOTE_YES && vote != VOTE_NO)
			{
				println("Invalid vote ", itos(vote), " for callvote ", itos(callvote), ", expected yes|no");
				return;
			}
			break;
		default:
			println("Invalid vote ", itos(vote), " for callvote ", itos(callvote));
			return;
	}

	string player_name = infokey(player, "name");
	if (player.voted != VOTE_IGNORED)
	{
		// this player initiated the vote, and their vote can be inferred from the callvote announcement
		writeByte(MSG_ALL, SVC_CGAMEPACKET);
		writeByte(MSG_ALL, EVENT_CALLVOTE_PLAYERVOTE);
		writeString(MSG_ALL, player_name);
		writeByte(MSG_ALL, vote);
	}

	player.voted = vote;

	// check if everyone voted so we can end the vote early
	int votes = 0;
	int players = 0;
	entity player = world;
	while (true)
	{
		player = find(player, classname, CLASS_PLAYER);
		if (player == world)
			break;

		if (strlen(infokey(player, "name")) == 0)
			continue;
		if (player.voted == VOTE_IGNORED || infokeyf(player, INFOKEY_P_SPECTATOR) != 0)
			continue;

		players++;
		if (player.voted != VOTE_NOTHING)
			votes++;
	}

	// everyone voted
	if (votes == players)
		Vote_Finish();
}

void Vote_OnCallVote(entity player, int vote, variant value)
{
	if (callvote != CALLVOTE_INACTIVE)
	{
		println("Another voting is currently active");
		return;
	}

	string player_name = infokey(player, "name");
	string value_str = "";
	if (vote == CALLVOTE_MODE)
	{
		println("Player '", player_name, "' initiated callvote mode ", (string)value);
		value_str = (string)value;
	}
	else if (vote == CALLVOTE_MAP)
	{
		println("Player '", player_name, "' initiated callvote map ", (string)value);
		value_str = (string)value;
	}
	else if (vote == CALLVOTE_FRAGLIMIT)
	{
		println("Player '", player_name, "' initiated callvote fraglimit ", itos((int)value));
		value_str = itos((int)value);
	}
	else if (vote == CALLVOTE_TIMELIMIT)
	{
		println("Player '", player_name, "' initiated callvote timelimit ", itos((int)value));
		value_str = itos((int)value);
	}
	else
	{
		println("Player '", player_name, "' tried to initiate unknown callvote ", itos(vote));
		return;
	}


	callvote = vote;
	callvote_value = value;
	callvote_endtime = time + callvote_timeout;

	writeByte(MSG_ALL, SVC_CGAMEPACKET);
	writeByte(MSG_ALL, EVENT_CALLVOTE);
	writeString(MSG_ALL, player_name);
	writeByte(MSG_ALL, callvote);
	writeString(MSG_ALL, value_str);

	// caller always votes yes
	Vote_OnPlayerVote(player, VOTE_YES, 0);
}

void CSEv_callvote_eis(entity player, int vote, string value)
{
	Vote_OnCallVote(player, vote, value);
}
void CSEv_callvote_eii(entity player, int vote, int value)
{
	Vote_OnCallVote(player, vote, value);
}
void CSEv_vote_ei(entity player, int vote)
{
	Vote_OnPlayerVote(player, vote, 0);
}
void CSEv_vote_eis(entity player, int vote, string map)
{
	Vote_OnPlayerVote(player, vote, map);
}
#endif