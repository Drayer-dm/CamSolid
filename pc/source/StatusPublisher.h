#pragma once

#include <functional>
#include <mutex>
#include "Status.h"

// Publica el estado de la cámara en memoria compartida para CamSolid.exe
class StatusPublisher
{
public:
    StatusPublisher();
    ~StatusPublisher();

    StatusPublisher(const StatusPublisher&) = delete;
    StatusPublisher& operator=(const StatusPublisher&) = delete;

    // Aplica "change" al estado y lo marca como recién actualizado
    void Update(const std::function<void(CamSolidStatus&)>& change);

private:
    std::mutex _mutex;
    HANDLE _mapping = nullptr;
    CamSolidStatus* _status = nullptr;
};
