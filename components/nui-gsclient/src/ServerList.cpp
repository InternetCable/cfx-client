/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include <nutsnbolts.h>
#include <CefOverlay.h>
#include <mmsystem.h>
#include <WS2tcpip.h>
#include <strsafe.h>
#include <fstream>
#include <array>

#include <rapidjson/document.h>
#include <rapidjson/writer.h>

#include <fnv.h>

#include <HttpClient.h>

#if defined(GTA_NY)
#define GS_GAMENAME "GTA4"
#elif defined(PAYNE)
#define GS_GAMENAME "Payne"
#elif defined(GTA_FIVE)
#define GS_GAMENAME "GTA5"
#else
#define GS_GAMENAME "CitizenFX"
#endif

template<typename TFunc>
void RequestInfoBlob(const std::string& server, const TFunc& cb)
{
	static HttpClient* httpClient = new HttpClient();

	std::string port = std::string(server);
	std::string ip = std::string(server);
	ip = ip.substr(0, ip.find(":"));
	port = port.substr(port.find(":") + 1);
	const char* portnum = port.c_str();

	if (port.empty())
	{
		portnum = "30120";
	}

	httpClient->DoGetRequest(ToWide(ip), atoi(portnum), L"/info.json", [=] (bool success, const char* data, size_t length)
	{
		if (!success)
		{
			cb("{}");
			return;
		}

		rapidjson::Document doc;
		doc.Parse(data, length);

		if (!doc.HasParseError())
		{
			auto member = doc.FindMember("version");

			if (member != doc.MemberEnd() && member->value.IsInt())
			{
				rapidjson::StringBuffer sbuffer;
				rapidjson::Writer<rapidjson::StringBuffer> writer(sbuffer);

				doc.Accept(writer);

				cb(sbuffer.GetString());

				uint64_t infoBlobKey = fnv1a_t<8>()(server);
				std::wstring blobPath = MakeRelativeCitPath(fmt::sprintf(L"cache\\servers\\%016llx.json", infoBlobKey));

				FILE* blobFile = _wfopen(blobPath.c_str(), L"w");

				if (blobFile)
				{
					fprintf(blobFile, "%s", sbuffer.GetString());
					fclose(blobFile);
				}
			}
		}
	});
}

template<typename TFunc>
void LoadInfoBlob(const std::string& server, int expectedVersion, const TFunc& cb)
{
	uint64_t infoBlobKey = fnv1a_t<8>()(server);
	std::wstring blobPath = MakeRelativeCitPath(fmt::sprintf(L"cache\\servers\\%016llx.json", infoBlobKey));
	
	FILE* blobFile = _wfopen(blobPath.c_str(), L"r");

	if (blobFile)
	{
		fseek(blobFile, 0, SEEK_END);

		int fOff = ftell(blobFile);

		fseek(blobFile, 0, SEEK_SET);

		std::vector<char> blob(fOff);
		fread(&blob[0], 1, blob.size(), blobFile);

		fclose(blobFile);

		rapidjson::Document doc;
		doc.Parse(blob.data(), blob.size());

		if (!doc.HasParseError())
		{
			auto member = doc.FindMember("version");

			if (member != doc.MemberEnd() && member->value.IsInt() && member->value.GetInt() == expectedVersion)
			{
				cb(std::string(blob.begin(), blob.end()));
				return;
			}
		}
	}

	RequestInfoBlob(server, cb);
}

struct gameserveritemext_t
{
	DWORD m_IP;
	WORD m_Port;
	DWORD queryTime;
	bool responded;
	bool queried;
	std::string m_hostName;
	int m_clients;
	int m_maxClients;
	int m_ping;
};

static struct
{
	SOCKET socket;
	sockaddr_in from;
	gameserveritemext_t servers[8192];
	int numServers;
	DWORD lastQueryStep;

	int curNumResults;

	DWORD queryTime;
} g_cls;

bool GSClient_Init()
{
	WSADATA wsaData;
	int err = WSAStartup(MAKEWORD(2, 2), &wsaData);

	if (err)
	{
		return false;
	}

	g_cls.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (g_cls.socket == INVALID_SOCKET)
	{
		trace("socket() failed - %d\n", WSAGetLastError());
		return false;
	}

	sockaddr_in bindAddr;
	memset(&bindAddr, 0, sizeof(bindAddr));
	bindAddr.sin_family = AF_INET;
	bindAddr.sin_port = 0;
	bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(g_cls.socket, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR)
	{
		trace("bind() failed - %d\n", WSAGetLastError());
		return false;
	}

	ULONG nonBlocking = 1;
	ioctlsocket(g_cls.socket, FIONBIO, &nonBlocking);

	setsockopt(g_cls.socket, SOL_SOCKET, SO_BROADCAST, (char*)&nonBlocking, sizeof(nonBlocking));

	return true;
}

gameserveritemext_t* GSClient_ServerItem(int i)
{
	return &g_cls.servers[i];
}

int GSClient_NumServers()
{
	return g_cls.numServers;
}

void GSClient_QueryServer(int i)
{
	gameserveritemext_t* server = &g_cls.servers[i];

	server->queried = true;
	server->responded = false;
	server->queryTime = timeGetTime();

	sockaddr_in serverIP;
	serverIP.sin_family = AF_INET;
	serverIP.sin_addr.s_addr = htonl(server->m_IP);
	serverIP.sin_port = htons(server->m_Port);

	char message[128];
	_snprintf(message, sizeof(message), "\xFF\xFF\xFF\xFFgetinfo xxx");

	sendto(g_cls.socket, message, strlen(message), 0, (sockaddr*)&serverIP, sizeof(serverIP));
}

void GSClient_QueryStep()
{
	if ((GetTickCount() - g_cls.lastQueryStep) < 50)
	{
		return;
	}

	g_cls.lastQueryStep = GetTickCount();

	int count = 0;

	for (int i = 0; i < g_cls.numServers && count < 20; i++)
	{
		if (!g_cls.servers[i].queried)
		{
			trace("query server %d %x\n", i, g_cls.servers[i].m_IP);

			GSClient_QueryServer(i);
			count++;
		}
	}
}

#define	BIG_INFO_STRING		8192  // used for system info key only
#define	BIG_INFO_KEY		  8192
#define	BIG_INFO_VALUE		8192

/*
===============
Info_ValueForKey

Searches the string for the given
key and returns the associated value, or an empty string.
FIXME: overflow check?
===============
*/
char *Info_ValueForKey(const char *s, const char *key)
{
	char	pkey[BIG_INFO_KEY];
	static	char value[2][BIG_INFO_VALUE];	// use two buffers so compares
											// work without stomping on each other
	static	int	valueindex = 0;
	char	*o;

	if (!s || !key)
	{
		return "";
	}

	if (strlen(s) >= BIG_INFO_STRING)
	{
		return "";
	}

	valueindex ^= 1;
	if (*s == '\\')
		s++;
	while (1)
	{
		o = pkey;
		while (*s != '\\')
		{
			if (!*s)
				return "";
			*o++ = *s++;
		}
		*o = 0;
		s++;

		o = value[valueindex];

		while (*s != '\\' && *s)
		{
			*o++ = *s++;
		}
		*o = 0;

		if (!_stricmp(key, pkey))
			return value[valueindex];

		if (!*s)
			break;
		s++;
	}

	return "";
}

void replaceAll(std::string& str, const std::string& from, const std::string& to)
{
	if (from.empty())
		return;
	size_t start_pos = 0;
	while ((start_pos = str.find(from, start_pos)) != std::string::npos)
	{
		str.replace(start_pos, from.length(), to);
		start_pos += to.length(); // In case 'to' contains 'from', like replacing 'x' with 'yx'
	}
}

void GSClient_HandleInfoResponse(const char* bufferx, int len)
{
	trace("received infoResponse\n");

	auto tempServer = std::make_shared<gameserveritemext_t>();
	tempServer->queryTime = timeGetTime();
	tempServer->m_IP = ntohl(g_cls.from.sin_addr.s_addr);
	tempServer->m_Port = ntohs(g_cls.from.sin_port);

	gameserveritemext_t* server = tempServer.get();

	for (int i = 0; i < g_cls.numServers; i++)
	{
		gameserveritemext_t* thisServer = &g_cls.servers[i];

		if ((thisServer->m_IP == ntohl(g_cls.from.sin_addr.s_addr)) && (thisServer->m_Port == ntohs(g_cls.from.sin_port)))
		{
			server = thisServer;
			break;
		}
	}

	bufferx++;

	char buffer[8192];
	strcpy(buffer, bufferx);

	g_cls.queryTime = timeGetTime();

	if (g_cls.curNumResults > 8192)
	{
		return;
	}

	int j = g_cls.curNumResults;

	g_cls.curNumResults++;

	server->m_ping = timeGetTime() - server->queryTime;
	server->m_maxClients = atoi(Info_ValueForKey(buffer, "sv_maxclients"));
	server->m_clients = atoi(Info_ValueForKey(buffer, "clients"));
	server->m_hostName = Info_ValueForKey(buffer, "hostname");

	server->m_IP = htonl(server->m_IP);

	replaceAll(server->m_hostName, "'", "\\'");

	const char* mapnameStr = Info_ValueForKey(buffer, "mapname");
	const char* gametypeStr = Info_ValueForKey(buffer, "gametype");

	std::string mapname;
	std::string gametype;

	if (mapnameStr)
	{
		mapname = mapnameStr;
	}

	if (gametypeStr)
	{
		gametype = gametypeStr;
	}

	replaceAll(mapname, "'", "\\'");
	replaceAll(gametype, "'", "\\'");

	std::array<char, 32> address;
	inet_ntop(AF_INET, &server->m_IP, address.data(), address.size());

	std::string addressStr = address.data();

	const char* infoBlobVersionString = Info_ValueForKey(buffer, "iv");

	auto onLoadCB = [=](const std::string& infoBlobJson)
	{
		nui::ExecuteRootScript(fmt::sprintf("citFrames['mpMenu'].contentWindow.postMessage({ type: 'serverAdd', name: '%s',"
			"mapname: '%s', gametype: '%s', clients: %d, maxclients: %d, ping: %d,"
			"addr: '%s:%d', infoBlob: %s }, '*');",
			server->m_hostName,
			mapname,
			gametype,
			server->m_clients,
			server->m_maxClients,
			server->m_ping,
			addressStr,
			server->m_Port,
			infoBlobJson));

		tempServer->m_IP = 0;
	};

	if (infoBlobVersionString && infoBlobVersionString[0])
	{
		std::string serverId = fmt::sprintf("%s:%d", addressStr, server->m_Port);
		int infoBlobVersion = atoi(infoBlobVersionString);

		LoadInfoBlob(serverId, infoBlobVersion, onLoadCB);
	}
	else
	{
		onLoadCB("{}");
	}

	// have over 60% of servers been shown?
	if (g_cls.curNumResults >= (g_cls.numServers * 0.6))
	{
		nui::ExecuteRootScript("citFrames['mpMenu'].contentWindow.postMessage({ type: 'refreshingDone' }, '*');");
	}
}

typedef struct
{
	unsigned char ip[4];
	unsigned short port;
} serverAddress_t;

void GSClient_HandleServersResponse(const char* buffer, int len)
{
	int numservers = 0;
	const char* buffptr = buffer;
	const char* buffend = buffer + len;
	serverAddress_t addresses[256];
	while (buffptr + 1 < buffend)
	{
		// advance to initial token
		do
		{
			if (*buffptr++ == '\\')
				break;
		} while (buffptr < buffend);

		if (buffptr >= buffend - 8)
		{
			break;
		}

		// parse out ip
		addresses[numservers].ip[0] = *buffptr++;
		addresses[numservers].ip[1] = *buffptr++;
		addresses[numservers].ip[2] = *buffptr++;
		addresses[numservers].ip[3] = *buffptr++;

		// parse out port
		addresses[numservers].port = (*(buffptr++)) << 8;
		addresses[numservers].port += (*(buffptr++)) & 0xFF;
		addresses[numservers].port = addresses[numservers].port;

		// syntax check
		if (*buffptr != '\\')
		{
			break;
		}

		numservers++;
		if (numservers >= 256)
		{
			break;
		}

		// parse out EOT
		if (buffptr[1] == 'E' && buffptr[2] == 'O' && buffptr[3] == 'T')
		{
			break;
		}
	}

	int count = g_cls.numServers;
	int max = 8192;

	for (int i = 0; i < numservers && count < max; i++)
	{
		// build net address
		unsigned int ip = (addresses[i].ip[0] << 24) | (addresses[i].ip[1] << 16) | (addresses[i].ip[2] << 8) | (addresses[i].ip[3]);
		//g_cls.servers[count].m_NetAdr.Init(ip, addresses[i].qport, addresses[i].port);
		g_cls.servers[count].m_IP = ip;
		g_cls.servers[count].m_Port = addresses[i].port;
		g_cls.servers[count].queried = false;

		count++;
	}

	g_cls.queryTime = timeGetTime();
	GSClient_QueryStep();

	g_cls.numServers = count;
}

#define CMD_GSR "getserversResponse"
#define CMD_INFO "infoResponse"

void GSClient_HandleOOB(const char* buffer, size_t len)
{
	if (!_strnicmp(buffer, CMD_GSR, strlen(CMD_GSR)))
	{
		GSClient_HandleServersResponse(&buffer[strlen(CMD_GSR)], len - strlen(CMD_GSR));
	}

	if (!_strnicmp(buffer, CMD_INFO, strlen(CMD_INFO)))
	{
		GSClient_HandleInfoResponse(&buffer[strlen(CMD_INFO)], len - strlen(CMD_INFO));
	}
}

void GSClient_PollSocket()
{
	char buf[2048];
	memset(buf, 0, sizeof(buf));

	sockaddr_in from;
	memset(&from, 0, sizeof(from));

	int fromlen = sizeof(from);

	while (true)
	{
		int len = recvfrom(g_cls.socket, buf, 2048, 0, (sockaddr*)&from, &fromlen);

		if (len == SOCKET_ERROR)
		{
			int error = WSAGetLastError();

			if (error != WSAEWOULDBLOCK)
			{
				trace("recv() failed - %d\n", error);
			}

			return;
		}

		g_cls.from = from;

		if (*(int*)buf == -1)
		{
			if (len < sizeof(buf))
			{
				buf[len] = '\0';
			}

			GSClient_HandleOOB(&buf[4], len - 4);
		}
	}
}

void GSClient_RunFrame()
{
	if (g_cls.socket)
	{
		GSClient_QueryStep();
		GSClient_PollSocket();
	}
}

void GSClient_QueryMaster()
{
	static bool lookedUp;
	static sockaddr_in masterIP;

	g_cls.queryTime = timeGetTime() + 15000;//(0xFFFFFFFF - 50000);

	g_cls.numServers = 0;

	if (!lookedUp)
	{
		hostent* host = gethostbyname("updater.fivereborn.com");

		if (!host)
		{
			trace("gethostbyname() failed - %d\n", WSAGetLastError());
			return;
		}

		masterIP.sin_family = AF_INET;
		masterIP.sin_addr.s_addr = *(ULONG*)host->h_addr_list[0];
		masterIP.sin_port = htons(30110);

		lookedUp = true;
	}

	g_cls.curNumResults = 0;

	nui::ExecuteRootScript("citFrames['mpMenu'].contentWindow.postMessage({ type: 'clearServers' }, '*');");

	char message[128];
	_snprintf(message, sizeof(message), "\xFF\xFF\xFF\xFFgetservers " GS_GAMENAME " 4 full empty");

	sendto(g_cls.socket, message, strlen(message), 0, (sockaddr*)&masterIP, sizeof(masterIP));

	for (int i = 30120; i < 30120 + 6; i++)
	{
		sockaddr_in broadcastIP = { 0 };
		broadcastIP.sin_family = AF_INET;
		broadcastIP.sin_port = htons(i);
		broadcastIP.sin_addr.s_addr = INADDR_BROADCAST;

		_snprintf(message, sizeof(message), "\xFF\xFF\xFF\xFFgetinfo xxx");
		sendto(g_cls.socket, message, strlen(message), 0, (sockaddr*)&broadcastIP, sizeof(broadcastIP));
	}
}

void GSClient_Refresh()
{
	if (!g_cls.socket)
	{
		GSClient_Init();
	}

	GSClient_QueryMaster();
}

void GSClient_GetFavorites()
{
	std::ifstream favFile(MakeRelativeCitPath(L"favorites.json"));
	std::string json;
	favFile >> json;
	favFile.close();

	nui::ExecuteRootScript(va("citFrames['mpMenu'].contentWindow.postMessage({ type: 'getFavorites', list: %s }, '*');", json));
}

void GSClient_SaveFavorites(const wchar_t *json)
{
	std::wofstream favFile(MakeRelativeCitPath(L"favorites.json"));
	favFile << json;
	favFile.close();
}

static InitFunction initFunction([] ()
{
	CreateDirectory(MakeRelativeCitPath(L"cache\\servers\\").c_str(), nullptr);

	nui::OnInvokeNative.Connect([] (const wchar_t* type, const wchar_t* arg)
	{
		if (!_wcsicmp(type, L"refreshServers"))
		{
			GSClient_Refresh();
		}

		if (!_wcsicmp(type, L"getFavorites"))
		{
			GSClient_GetFavorites();
		}

		if (!_wcsicmp(type, L"saveFavorites"))
		{
			GSClient_SaveFavorites(arg);
		}
	});

	std::thread([] ()
	{
		while (true)
		{
			Sleep(1);

			GSClient_RunFrame();
		}
	}).detach();
});
