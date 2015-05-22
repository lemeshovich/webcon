#include "extension.h"

#include "CDetour/detours.h"

#include <fcntl.h>

#ifdef _WIN32
#include <io.h>
#include <Winsock2.h>
#include <Ws2tcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#define closesocket close
#define ioctlsocket ioctl
#define WSAGetLastError() errno
#endif

#if defined(_MSC_FULL_VER) && !defined (_SSIZE_T_DEFINED)
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif // !_SSIZE_T_DEFINED */

// tier1 supremecy
#include "utlvector.h"
#include "netadr.h"

#include "sm_namehashset.h"

Conplex g_Conplex;
SMEXT_LINK(&g_Conplex);

IGameConfig *gameConfig;

bool shouldHandleProcessAccept;

CDetour *detourProcessAccept;
CDetour *detourRunFrame;

struct ProtocolHandler
{
	static bool matches(const char *key, const ProtocolHandler &value);

	ProtocolHandler(const char *id, IConplex::ProtocolDetectorCallback detector, IConplex::ProtocolHandlerCallback handler);
	~ProtocolHandler();

	// TODO: Once we update to a version of SM with modern AMTL,
	// this needs to be converted to a move constructor.
	// (and the copy ctor deletes below can go)
	ProtocolHandler(ke::Moveable<ProtocolHandler> other);

	ProtocolHandler(ProtocolHandler const &other) = delete;
	ProtocolHandler &operator =(ProtocolHandler const &other) = delete;

	char *id;
	IConplex::ProtocolDetectorCallback detector;
	IConplex::ProtocolHandlerCallback handler;
};

bool ProtocolHandler::matches(const char *key, const ProtocolHandler &value)
{
	return (strcmp(key, value.id) == 0);
}

ProtocolHandler::ProtocolHandler(const char *id, IConplex::ProtocolDetectorCallback detector, IConplex::ProtocolHandlerCallback handler)
{
	this->id = strdup(id);
	this->detector = detector;
	this->handler = handler;
}

ProtocolHandler::ProtocolHandler(ke::Moveable<ProtocolHandler> other)
{
	id = other->id;
	detector = other->detector;
	handler = other->handler;

	other->id = NULL;
}

ProtocolHandler::~ProtocolHandler()
{
	free(id);
}

NameHashSet<ProtocolHandler> protocolHandlers;

struct PendingSocket
{
	int timeout;
	int socket;
	sockaddr socketAddress;
	socklen_t socketAddressLength;
};

CUtlVector<PendingSocket> pendingSockets;

struct ISocketCreatorListener
{
	virtual bool ShouldAcceptSocket(int socket, const netadr_t &address) = 0; 
	virtual void OnSocketAccepted(int socket, const netadr_t &address, void **data) = 0; 
	virtual void OnSocketClosed(int socket, const netadr_t &address, void *data) = 0;
};

struct CRConServer: public ISocketCreatorListener
{
	static void *HandleFailedRconAuthFunction;
	bool HandleFailedRconAuth(const netadr_t &address);
};

CRConServer *rconServer;

void *CRConServer::HandleFailedRconAuthFunction = NULL;

bool CRConServer::HandleFailedRconAuth(const netadr_t &address)
{
	if (!CRConServer::HandleFailedRconAuthFunction) {
		return false;
	}

#ifdef _WIN32
	return ((bool (__fastcall *)(CRConServer *, void *, const netadr_t &))CRConServer::HandleFailedRconAuthFunction)(this, NULL, address);
#else
	return ((bool (*)(CRConServer *, const netadr_t &))CRConServer::HandleFailedRconAuthFunction)(this, address);
#endif
}

struct CSocketCreator 
{
	// These are our own functions, they're in here for convenient access to the engine's CSocketCreator variables.
	void ProcessAccept();
	void HandSocketToEngine(int socket, const sockaddr *socketAddress);

	struct AcceptedSocket
	{
		int socket;
		netadr_t address;
		void *data;
	};

	ISocketCreatorListener *listener;
	CUtlVector<AcceptedSocket> acceptedSockets;
	int listenSocket;
	netadr_t listenAddress;
};

CSocketCreator *socketCreator;

void CSocketCreator::ProcessAccept()
{
	sockaddr socketAddress;
	socklen_t socketAddressLength = sizeof(socketAddress);
	int socket = accept(listenSocket, &socketAddress, &socketAddressLength);
	if (socket == -1) {
		return;
	}

	rootconsole->ConsolePrint("(%d) New listen socket accepted.", socket);

	int opt = 1;
	setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (char *)&opt, sizeof(opt)); 

	opt = 1;
	setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));

	opt = 1;
	if (ioctlsocket(socket, FIONBIO, (unsigned long *)&opt) == -1) {
		rootconsole->ConsolePrint("(%d) Failed to set socket options.", socket);
		closesocket(socket);
		return;
	}

	netadr_t address;
	address.SetFromSockadr(&socketAddress);

	if (listener && !listener->ShouldAcceptSocket(socket, address)) {
		rootconsole->ConsolePrint("(%d) Listener rejected connection.", socket);
		closesocket(socket);
		return;
	}

	PendingSocket *pendingSocket = &pendingSockets[pendingSockets.AddToTail()];
	pendingSocket->timeout = 0;
	pendingSocket->socket = socket;
	pendingSocket->socketAddress = socketAddress;
	pendingSocket->socketAddressLength = socketAddressLength;
}

void CSocketCreator::HandSocketToEngine(int socket, const sockaddr *socketAddress)
{
	netadr_t address;
	address.SetFromSockadr(socketAddress);

	AcceptedSocket *acceptedSocket = &acceptedSockets[acceptedSockets.AddToTail()];
	acceptedSocket->socket = socket;
	acceptedSocket->address = address;
	acceptedSocket->data = NULL;

	if (listener) {
		listener->OnSocketAccepted(acceptedSocket->socket, acceptedSocket->address, &(acceptedSocket->data));
	}
}

bool SocketWouldBlock() {
#if _WIN32
	return (WSAGetLastError() == WSAEWOULDBLOCK);
#else
	return (errno == EAGAIN || errno == EWOULDBLOCK);
#endif
}

DETOUR_DECL_MEMBER0(ProcessAccept, void)
{
	if (!shouldHandleProcessAccept) {
		return DETOUR_MEMBER_CALL(ProcessAccept)();
	}

	socketCreator = (CSocketCreator *)this;

	// Check for incoming sockets first.
	socketCreator->ProcessAccept();

	unsigned char buffer[32] = {};

	int count = pendingSockets.Count();
	for (int i = (count - 1); i >= 0; --i) {
		PendingSocket *pendingSocket = &pendingSockets[i];

		ssize_t ret = recv(pendingSocket->socket, (char *)buffer, sizeof(buffer), MSG_PEEK);

		if (ret == 0) {
			rootconsole->ConsolePrint("(%d) Listen socket closed.", pendingSocket->socket);
			closesocket(pendingSocket->socket);

			pendingSockets.Remove(i);
			continue;
		}

		if (ret == -1 && !SocketWouldBlock()) {
			rootconsole->ConsolePrint("(%d) recv error: %d", WSAGetLastError());
			closesocket(pendingSocket->socket);

			pendingSockets.Remove(i);
			continue;
		}
		
		int pendingCount = 0;
		const ProtocolHandler *handler = NULL;
		
		if (ret > 0)
		{
			// TODO: Don't call handlers that have returned NoMatch already on a previous call for this connection.
			for (NameHashSet<ProtocolHandler>::iterator i = protocolHandlers.iter(); !i.empty(); i.next()) {
				IConplex::ProtocolDetectionState state = i->detector(i->id, buffer, ret);
				rootconsole->ConsolePrint(">>> %s = %d %d", i->id, ret, state);
				
				if (state == IConplex::Match) {
					handler = &(*i);
					break;
				}
				
				if (state == IConplex::NeedMoreData) {
					pendingCount++;
					continue;
				}
			}
		}

		if (!handler) {
			// Ran out of handlers or data.
			if ((ret > 0 && pendingCount == 0) || ret == sizeof(buffer)) {
				if (rconServer) {
					netadr_t address;
					address.SetFromSockadr(&(pendingSocket->socketAddress));
					rconServer->HandleFailedRconAuth(address);
				}

				rootconsole->ConsolePrint("(%d) Unidentified protocol on socket.", pendingSocket->socket);
				closesocket(pendingSocket->socket);

				pendingSockets.Remove(i);
				continue;
			}
		
			pendingSocket->timeout++;

			// About 15 seconds.
			if (pendingSocket->timeout > 1000) {
				if (rconServer) {
					// TODO: We need logic to exclude clients connected to the HTTP server.
					//rconServer->HandleFailedRconAuth(pendingSocket->address);
				}

				rootconsole->ConsolePrint("(%d) Listen socket timed out.", pendingSocket->socket);
				closesocket(pendingSocket->socket);

				pendingSockets.Remove(i);
			}

			continue;
		}

		if (handler->handler(handler->id, pendingSocket->socket, &(pendingSocket->socketAddress), pendingSocket->socketAddressLength)) {
			rootconsole->ConsolePrint("(%d) Gave %s socket to handler.", pendingSocket->socket, handler->id);
		} else {
			rootconsole->ConsolePrint("(%d) %s handler rejected socket.", pendingSocket->socket, handler->id);
			closesocket(pendingSocket->socket);
		}
		
		pendingSockets.Remove(i);
	}
}

DETOUR_DECL_MEMBER0(RunFrame, void)
{
	rconServer = (CRConServer *)this;

	shouldHandleProcessAccept = true;
	DETOUR_MEMBER_CALL(RunFrame)();
	shouldHandleProcessAccept = false;
}

IConplex::ProtocolDetectionState ConplexRConDetector(const char *id, const unsigned char *buffer, unsigned int bufferLength)
{
	if (bufferLength <= 2) return IConplex::NeedMoreData;
	if (buffer[2] != 0x00) return IConplex::NoMatch;
	if (bufferLength <= 3) return IConplex::NeedMoreData;
	if (buffer[3] != 0x00) return IConplex::NoMatch;
	if (bufferLength <= 8) return IConplex::NeedMoreData;
	if (buffer[8] != 0x03) return IConplex::NoMatch;
	if (bufferLength <= 9) return IConplex::NeedMoreData;
	if (buffer[9] != 0x00) return IConplex::NoMatch;
	if (bufferLength <= 10) return IConplex::NeedMoreData;
	if (buffer[10] != 0x00) return IConplex::NoMatch;
	if (bufferLength <= 11) return IConplex::NeedMoreData;
	if (buffer[11] != 0x00) return IConplex::NoMatch;
	return IConplex::Match;
}

bool ConplexRConHandler(const char *id, int socket, const sockaddr *address, unsigned int addressLength)
{
	if (!socketCreator) {
		return false;
	}
	
	socketCreator->HandSocketToEngine(socket, address);
	return true;
}

bool Conplex::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	if (!sharesys->AddInterface(myself, this))
	{
		strncpy(error, "Could not add IConplex interface", maxlength);
		return false;
	}
	
	if (!gameconfs->LoadGameConfigFile("conplex.games", &gameConfig, error, maxlength)) {
		return false;
	}

	CDetourManager::Init(smutils->GetScriptingEngine(), gameConfig);

	detourProcessAccept = DETOUR_CREATE_MEMBER(ProcessAccept, "ProcessAccept");
	if (!detourProcessAccept) {
		strncpy(error, "Error setting up ProcessAccept detour", maxlength);
		gameconfs->CloseGameConfigFile(gameConfig);
		return false;
	}

	detourRunFrame = DETOUR_CREATE_MEMBER(RunFrame, "RunFrame");
	if (!detourRunFrame) {
		shouldHandleProcessAccept = true;
		smutils->LogError(myself, "WARNING: Error setting up RunFrame detour, all TCP sockets will be hooked.");
	}

	if (!gameConfig->GetMemSig("HandleFailedRconAuth", &CRConServer::HandleFailedRconAuthFunction)) {
		smutils->LogError(myself, "WARNING: HandleFailedRconAuth not found in gamedata, bad clients will not be banned.");
	} else if (!CRConServer::HandleFailedRconAuthFunction) {
		smutils->LogError(myself, "WARNING: Scan for HandleFailedRconAuth failed, bad clients will not be banned.");
	}

	detourProcessAccept->EnableDetour();
	
	if (detourRunFrame) {
		detourRunFrame->EnableDetour();
	}
	
	RegisterProtocolHandler("RCon", ConplexRConDetector, ConplexRConHandler);

	return true;
}

void Conplex::SDK_OnUnload()
{
	if (detourRunFrame) {
		detourRunFrame->DisableDetour();
	}
	
	detourProcessAccept->DisableDetour();

	gameconfs->CloseGameConfigFile(gameConfig);
}

unsigned int Conplex::GetInterfaceVersion()
{
	return SMINTERFACE_CONPLEX_VERSION;
}

const char *Conplex::GetInterfaceName()
{
	return SMINTERFACE_CONPLEX_NAME;
}

bool Conplex::RegisterProtocolHandler(const char *id, ProtocolDetectorCallback detector, ProtocolHandlerCallback handler)
{
	NameHashSet<ProtocolHandler>::Insert i = protocolHandlers.findForAdd(id);

	if (i.found()) {
		return false;
	}

	ProtocolHandler ph(id, detector, handler);
	return protocolHandlers.add(i, ke::Moveable<ProtocolHandler>(ph));
}

bool Conplex::DropProtocolHandler(const char *id)
{
	return protocolHandlers.remove(id);
}
