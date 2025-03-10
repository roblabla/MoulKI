#include "MoulKI.h"
#include "qtAuthClient.h"

qtAuthClient::qtAuthClient(MoulKI* ki) : QObject(ki),
    pnAuthClient(ki->getResManager()), parent(ki), currentPlayerId(0) {
    setKeys(ki->Keys.Auth.X, ki->Keys.Auth.N);
    if(ki->Keys.Auth.G != 0)
        setKeyG(ki->Keys.Auth.G);
    setClientInfo(BUILD_NUMBER, 50, 1, s_moulUuid);
}

qtAuthClient::~qtAuthClient() {
}

void qtAuthClient::startLogin(QString user, QString pass) {
    players.clear();
    // apparently HSPlasma still doesn't lowercase the username
    this->user = user.toLower().toUtf8().constData();
    this->pass = pass.toUtf8().constData();
    setStatus("Connecting...");
    if(pnAuthClient::connect(parent->Host.toStdString().data(), parent->Port) != kNetSuccess) {
        setStatus("Error Connecting To Server");
        return;
    }
    sendClientRegisterRequest();
}

void qtAuthClient::setPlayer(uint32_t playerId) {
    currentPlayerId = playerId;
    sendAcctSetPlayerRequest(playerId);
}

void qtAuthClient::onClientRegisterReply(uint32_t serverChallenge) {
    setStatus("Authenticating...");
    sendAcctLoginRequest(serverChallenge, rand(), user, pass);
}

void qtAuthClient::onAcctPlayerInfo(uint32_t, uint32_t playerId, const ST::string& playerName, const ST::string& avatarModel, uint32_t) {
    authPlayer player;
    player.ID = playerId;
    player.Name = playerName;
    player.avatar = avatarModel;
    players.append(player);
    qWarning("Added player %s (%u)", playerName.c_str(), playerId);
}

void qtAuthClient::onAcctLoginReply(uint32_t, ENetError result,
        const plUuid& acctUuid, uint32_t, uint32_t,
        const uint32_t* encryptKey) {
    if (result != kNetSuccess) {
        setStatus(ST::format("Auth Failed ({})",
                    GetNetErrorString(result)));
        return;
    }

    setStatus("Auth Successful");
    this->acctUuid = acctUuid;
    this->sendFileListRequest("SDL", "sdl");

    emit gotEncKeys(encryptKey[0], encryptKey[1],
            encryptKey[2], encryptKey[3]);
    emit loginSuccessful();
}

void qtAuthClient::onAcctSetPlayerReply(uint32_t, ENetError) {
    parent->vault.queueRoot(currentPlayerId);
    sendVaultNodeFetch(currentPlayerId);
}

void qtAuthClient::onPublicAgeList(uint32_t, ENetError result, size_t count, const pnNetAgeInfo* ages) {
    if(result != kNetSuccess) {
        setStatus(ST::format("Get Public Ages Failed ({})",
                    GetNetErrorString(result)));
        return;
    }

    setStatus(ST::format("Got {} Public Ages", count));

    QList< QPair<QString, plUuid> > publicAges;
    for(size_t i = 0; i < count; i++) {
        publicAges.append(QPair<QString, plUuid>(ages[i].fAgeFilename.c_str(), ages[i].fAgeInstanceId));
    }

    emit gotPublicAges(publicAges);
}

void qtAuthClient::onFileListReply(uint32_t, ENetError,
        size_t count, const pnAuthFileItem* files) {

    pendingSdlFiles.clear();
    pendingSdlFiles.reserve(count);
    for (size_t i = 0; i < count; i++) {
        pendingSdlFiles.append(files[i]);
    }
    currentPendingSdlFile = 0;
    downloadNextSdlFile();
}

void qtAuthClient::onFileDownloadChunk(uint32_t transId, ENetError result,
        uint32_t totalSize, uint32_t chunkOffset, size_t chunkSize,
        const unsigned char* chunkData) {
    if (result != kNetSuccess) {
        setStatus(ST::format("File download failed ({})",
                    GetNetErrorString(result)));
        return;
    }

    hsStream* S = sdlFiles[transId];
    S->write(chunkSize, chunkData);

    if (chunkOffset + chunkSize == totalSize) {
        S->rewind();
        qWarning("Successfully downloaded a %d byte file.", totalSize);
        downloadNextSdlFile();
        emit gotSDLFile(S);
    }
}

void qtAuthClient::downloadNextSdlFile() {
    if(currentPendingSdlFile == pendingSdlFiles.size()) {
        qWarning("Done downloading SDL files.");
        currentPendingSdlFile = 0;
        pendingSdlFiles.clear();
    }else{
        qWarning("Downloading file %s (%d of %d, %d bytes)",
                pendingSdlFiles[currentPendingSdlFile].fFilename.c_str(), currentPendingSdlFile+1,
                pendingSdlFiles.size(), pendingSdlFiles[currentPendingSdlFile].fFileSize);
        uint32_t fileTrans;
        fileTrans = this->sendFileDownloadRequest(pendingSdlFiles[currentPendingSdlFile].fFilename);
        sdlFiles.insert(fileTrans, new hsRAMStream(PlasmaVer::pvMoul));
        currentPendingSdlFile++;
    }
}

void qtAuthClient::onVaultNodeRefsFetched(uint32_t, ENetError, size_t count, const pnVaultNodeRef* refs) {
    for(unsigned int i = 0; i < count; i++) {
        setStatus(ST::format("Ref: {{{} -> {}} {}", refs[i].fParent, refs[i].fChild, refs[i].fOwner));
        parent->vault.addRef(refs[i]);
        if(!fetchQueue.contains(refs[i].fChild)) {
            fetchQueue.append(refs[i].fChild);
            sendVaultNodeFetch(refs[i].fChild);
        }
        if(!fetchQueue.contains(refs[i].fParent)) {
            fetchQueue.append(refs[i].fParent);
            sendVaultNodeFetch(refs[i].fParent);
        }
    }
}

void qtAuthClient::onVaultNodeFetched(uint32_t, ENetError, const pnVaultNode& node) {
    setStatus(ST::format("Node: ({})", node.getNodeIdx()));
    parent->vault.addNode(node);
}

void qtAuthClient::onVaultNodeChanged(uint32_t nodeId, const plUuid&) {
    if(!fetchQueue.contains(nodeId))
        fetchQueue.append(nodeId);
    sendVaultNodeFetch(nodeId);
}

void qtAuthClient::onVaultNodeAdded(uint32_t parent, uint32_t child, uint32_t owner) {
    pnVaultNodeRef ref;
    ref.fParent = parent;
    ref.fChild = child;
    ref.fOwner = owner;
    setStatus(ST::format("Ref: {{{} -> {}} {}", ref.fParent, ref.fChild, ref.fOwner));
    this->parent->vault.addRef(ref);
    if(!fetchQueue.contains(parent)) {
        fetchQueue.append(parent);
        sendVaultNodeFetch(parent);
        sendVaultFetchNodeRefs(parent);
    }
    if(!fetchQueue.contains(child)) {
        fetchQueue.append(child);
        sendVaultNodeFetch(child);
        sendVaultFetchNodeRefs(child);
    }
}

void qtAuthClient::onVaultNodeRemoved(uint32_t parent, uint32_t child) {
    setStatus(ST::format("UnRef: {{{} -> {}}", parent, child));
    this->parent->vault.removeRef(parent, child);
}

void qtAuthClient::onVaultNodeCreated(uint32_t transId, ENetError result, uint32_t nodeId) {
    if(result == kNetSuccess) {
        foreach(queuedRef ref, refQueue) {
            if(ref.fTransId == transId) {
                sendVaultNodeAdd(ref.fParent, nodeId, 0);
                refQueue.removeAll(ref);
                break;
            }
        }
    }else{
        setStatus(ST::format("Create Node Failed ({})", GetNetErrorString(result)));
    }
}

void qtAuthClient::queueRef(uint32_t transId, uint32_t parent) {
    queuedRef ref;
    ref.fTransId = transId;
    ref.fParent = parent;
    refQueue.append(ref);
}

bool qtAuthClient::queuedRef::operator ==(const qtAuthClient::queuedRef& ref) {
    return fTransId == ref.fTransId;
}

void qtAuthClient::setStatus(ST::string msg) {
    emit sigStatus(std::move(msg));
}

void qtAuthClient::onVaultNodeFindReply(uint32_t, ENetError result, size_t count, const uint32_t *nodes) {
    if(result == kNetSuccess) {
        setStatus(ST::format("Found {} Nodes", count));
        // it's not safe to send an array allocated elsewhere via a queued signal
        QList<uint32_t> nodeList;
        for(uint32_t i = 0; i < count; i++) {
            nodeList.append(nodes[i]);
        }
        emit foundNodes(nodeList);
    }else{
        setStatus(ST::format("Find Node Failure: ({})", GetNetErrorString(result)));
    }
}

void qtAuthClient::onVaultAddNodeReply(uint32_t, ENetError result) {
    if(result == kNetSuccess) {
        setStatus("Add Node Successful");
    }else{
        setStatus(ST::format("Add Node Failure: ({})", GetNetErrorString(result)));
    }
}

void qtAuthClient::onVaultSaveNodeReply(uint32_t transId, ENetError result) {
    if(result == kNetSuccess) {
        setStatus("Save Node Successful");
        emit saveNodeSuccessful(transId);
    }else{
        setStatus(ST::format("Save Node Failure: ({})", GetNetErrorString(result)));
    }
}

void qtAuthClient::onVaultRemoveNodeReply(uint32_t, ENetError result) {
    if(result == kNetSuccess) {
        setStatus("Remove Node Successful");
    }else{
        setStatus(ST::format("Remove Node Failure: ({})", GetNetErrorString(result)));
    }
}

void qtAuthClient::onAgeReply(uint32_t, ENetError result, uint32_t mcpId, const plUuid &ageInstanceId, uint32_t ageVaultId, uint32_t gameServerAddress) {
    if(result == kNetSuccess) {
        setStatus("Age Request Successful");
        emit gotAge(gameServerAddress, ageInstanceId, mcpId, ageVaultId);
    }else{
        setStatus(ST::format("Age Request Failed: ({})", GetNetErrorString(result)));
    }
}
