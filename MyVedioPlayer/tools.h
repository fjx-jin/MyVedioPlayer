#ifndef TOOL_H
#define TOOL_H

#include <string>
#include <time.h>

static std::string getTime()
{
	const char* fmt = "%Y-%M-%D %H:%m:%S";
	time_t t = time(nullptr);
	char time_str[64];
	strftime(time_str, sizeof(time_str), fmt, localtime(&t));

	return time_str;
}

#define LOGINFO(format, ...) fprintf(stdout,"[I]%s [%s::%d %s] " format "\n",getTime().data(), __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#define LOGERR(format, ...) fprintf(stdout,"[I]%s [%s::%d %s] " format "\n",getTime().data(), __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#endif // !TOOL_H

