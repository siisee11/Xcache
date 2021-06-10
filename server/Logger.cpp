#include "Logger.h"

Logger::Logger()
{
	this->logLevel = LOG_LEVEL_ERROR;
}

Logger::Logger(int level)
{
	this->logLevel = level;

	FILE *fp = NULL;
	fp = fopen(__LOG_FILE__, "w");

	if(fp == NULL)
	{
		puts("fail to open file pointer");
		return;
	}

	if(fp != NULL)
	{
		fclose(fp);
	}
}

string Logger::getTimestamp()
{
	string result;
	time_t currentSec = time(NULL);
	tm *t = localtime(&currentSec);
	ostringstream oss;

	switch(t->tm_mon)
	{
		case(0): result = "Jan"; break;
		case(1): result = "Feb"; break;
		case(2): result = "Mar"; break;
		case(3): result = "Apr"; break;
		case(4): result = "May"; break;
		case(5): result = "Jun"; break;
		case(6): result = "Jul"; break;
		case(7): result = "Aug"; break;
		case(8): result = "Sep"; break;
		case(9): result = "Oct"; break;
		case(10): result = "Nov"; break;
		case(11): result = "Dec"; break;
	}

	oss.clear();
	oss << " " << setfill('0') << setw(2) << t->tm_mday << " " << t->tm_year+1900;
	oss << " " << setfill('0') << setw(2) << t->tm_hour;
	oss << ":" << setfill('0') << setw(2) << t->tm_min;
	oss << ":" << setfill('0') << setw(2) << t->tm_sec << '\0';

	result = result + oss.str();

	return result;
}

void Logger::writeLog(const char *funcName, int line, int lv, const char *str, ...)
{
	FILE *fp = NULL;
	fp = fopen(__LOG_FILE__, "a");

	if(fp == NULL)
	{
		puts("fail to open file pointer");
		return;
	}

	char *result = NULL;
	char level[10];

	switch(lv)
	{
		case(LOG_LEVEL_FATAL): strcpy(level, "[FATAL]"); break;
		case(LOG_LEVEL_ERROR): strcpy(level, "[ERROR]"); break;
		case(LOG_LEVEL_WARN): strcpy(level, "[WARN] "); break;
		case(LOG_LEVEL_INFO): strcpy(level, "[INFO] "); break;
		case(LOG_LEVEL_DEBUG): strcpy(level, "[DEBUG]"); break;
		case(LOG_LEVEL_TRACE): strcpy(level, "[TRACE]"); break;
	}

	result = (char*)malloc(sizeof(char)*(21+strlen(funcName)+strlen(str)+30));
	sprintf(result, "%s %s [%s:%d] : %s\n", level, getTimestamp().c_str(), funcName, line, str);

	va_list args;

	va_start(args, str);
	vfprintf(fp, result, args);
	va_end(args);

	va_start(args, str);
	if(this->logLevel >= lv)
	{
		vprintf(result, args);
	}
	va_end(args);

	if(result != NULL)
	{
		free(result);
	}
	if(fp != NULL)
	{
		fclose(fp);
	}

	return;
}
