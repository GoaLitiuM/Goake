const float CHAT_SHOW_TIME = 10.0f;

enum
{
	CHAT_TYPE_SYSTEM = 0,
	CHAT_TYPE_MESSAGE,
	CHAT_TYPE_DEATH,
	CHAT_TYPE_SPAM,
	CHAT_TYPE_RACE,
};

struct ChatLine
{
	string timestamp;	// FIXME: should be server timestamp
	int time;			// received time
	string source;
	int type;
	string msg;
};

ChatLine* chatlines;
int chatlines_allocated_count;
int chatlines_count;

ChatLine* Chat_NewLine()
{
	if (chatlines_count == chatlines_allocated_count)
	{
		int alloc_size = chatlines_allocated_count * 2;
		if (alloc_size == 0)
			alloc_size = 128;

		ChatLine* ptr = memalloc(sizeof(ChatLine) * alloc_size);
		chatlines_allocated_count = alloc_size;
		if (chatlines)
		{
			for (int i=0; i<chatlines_count; i++)
				memcpy(&ptr[i], &chatlines[i], sizeof(ChatLine));

			memfree(chatlines);
		}
		chatlines = ptr;
	}

	ChatLine* line = &chatlines[chatlines_count];
	chatlines_count++;
	return line;
}

void Chat_SystemMessage(string msg, int type = CHAT_TYPE_SYSTEM)
{
	ChatLine* line = Chat_NewLine();
	line->timestamp = strftime(1, "%H:%M:%S");
	line->time = cltime;
	line->source = "";
	line->type = CHAT_TYPE_SYSTEM;
	line->msg = msg;

	println(msg);
}

void Chat_Message(string timestamp, string source, string msg)
{
	ChatLine* line = Chat_NewLine();
	line->timestamp = timestamp;
	line->time = cltime;
	line->source = source;
	line->type = CHAT_TYPE_MESSAGE;
	line->msg = msg;

	println(source, ": ", msg);
}

void Chat_OnPrint(string printmsg)
{
	int msg_start = strstrofs(printmsg, ":");
	int len = strlen(printmsg);

	string source = strdecolorize(substring(printmsg, 0, msg_start));
	string msg = substring(printmsg, msg_start + 2, len - (msg_start + 3));

	// FIXME: we can't possibly know the real time this message was sent
	string timestamp = strftime(1, "%H:%M:%S");

	Chat_Message(timestamp, source, msg);

	if (cvar("cl_chatsound") != 0)
	{
		// TODO: detect enemy and teammates
		string chatsound = cvar_string("cl_enemychatsound");
		if (!chatsound)
			chatsound = cvar_string("cl_teamchatsound");
		clientsound(self, CHAN_AUTO, chatsound, 1, 0);
	}
}

void Hud_Chat(float width, float height)
{
	int spacing = font_chat->fontsize;
	int linenum = 0;
	float left = 8;
	float bottom = (height*0.8);
	int padding = 2;
	int line_width = width*0.3;
	int maxlines = 8;

	bool chat_open = getkeydest() == 1;
	bool chat_timestamps = cvar("hud_chat_timestamps") > 0;
	bool misc_timestamps = cvar("hud_chat_timestamps") == 1;

	int lines_total = 0;
	for (int i=0; i<chatlines_count; i++)
	{
		ChatLine* line = &chatlines[i];
		if (!chat_open && cltime > line->time+CHAT_SHOW_TIME)
			continue;
		lines_total++;
	}

	if (lines_total == 0 && !chat_open)
		return;
	else
	{
		int background_height = lines_total*spacing;
		drawfill([left-padding, bottom-padding-background_height, 0], [line_width + 2*padding, background_height + 2*padding + chat_open*spacing, 0], [0, 0, 0], 0.66);
	}

	if (lines_total > 0)
	{
		float offset_y = bottom-spacing+padding;

		for (int i=chatlines_count-1; i>=0; i--)
		{
			ChatLine* line = &chatlines[i];
			if (!chat_open && cltime > line->time+CHAT_SHOW_TIME)
				continue;

			if (linenum >= maxlines)
				break;

			float offset_x = 0.0f;

			if ((line->type == CHAT_TYPE_MESSAGE && chat_timestamps) ||
				(line->type != CHAT_TYPE_MESSAGE && misc_timestamps))
			{
				string timestamp_str = strcat(line->timestamp, " ");
				DrawText(font_chat, [left + offset_x, offset_y, 0], timestamp_str, '1 1 1', 1);
				offset_x += TextWidth(font_chat, timestamp_str, false);
			}

			if (line->type == CHAT_TYPE_MESSAGE)
			{
				string source_str = line->source;
				string msg_str = strcat(": ", line->msg);

				DrawText(font_chat_bold, [left + offset_x, offset_y, 0], source_str, '1 1 1', 1);
				offset_x += TextWidth(font_chat_bold, source_str, false);

				DrawText(font_chat, [left + offset_x, offset_y, 0], msg_str, '1 1 1', 1);
			}
			else
				DrawText(font_chat, [left + offset_x, offset_y, 0], line->msg, '1 1 1', 1);

			linenum++;
			offset_y -= spacing;
		}
	}

	if (chat_open)
	{
		string chat_buffer = getchatbuffer();
		//int chat_bufferpos = getchatbufferpos();
		DrawText(font_chat, [left, bottom+padding, 0], strcat(chat_team ? "TEAM" : "", "> ", chat_buffer), '1 1 1', 1);
	}
}