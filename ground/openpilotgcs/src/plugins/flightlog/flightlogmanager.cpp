/**
 ******************************************************************************
 *
 * @file       flightlogmanager.cpp
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2012.
 * @addtogroup [Group]
 * @{
 * @addtogroup FlightLogManager
 * @{
 * @brief [Brief]
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "flightlogmanager.h"
#include "extensionsystem/pluginmanager.h"

#include <QApplication>

#include "debuglogcontrol.h"
#include "uavobjecthelper.h"

FlightLogManager::FlightLogManager(QObject *parent) :
    QObject(parent), m_disableControls(false)
{
    ExtensionSystem::PluginManager *pm = ExtensionSystem::PluginManager::instance();

    m_objectManager    = pm->getObject<UAVObjectManager>();
    Q_ASSERT(m_objectManager);

    m_flightLogControl = DebugLogControl::GetInstance(m_objectManager);
    Q_ASSERT(m_flightLogControl);

    m_flightLogStatus  = DebugLogStatus::GetInstance(m_objectManager);
    Q_ASSERT(m_flightLogStatus);
    connect(m_flightLogStatus, SIGNAL(FlightChanged(quint16)), this, SLOT(updateFlightEntries(quint16)));

    m_flightLogEntry   = DebugLogEntry::GetInstance(m_objectManager);
    Q_ASSERT(m_flightLogEntry);

    updateFlightEntries(m_flightLogStatus->getFlight());
}

FlightLogManager::~FlightLogManager()
{
    while (!m_logEntries.isEmpty()) {
        delete m_logEntries.takeFirst();
    }
}

void addLogEntries(QQmlListProperty<ExtendedDebugLogEntry> *list, ExtendedDebugLogEntry *entry)
{
    Q_UNUSED(list);
    Q_UNUSED(entry);
}

int countLogEntries(QQmlListProperty<ExtendedDebugLogEntry> *list)
{
    return static_cast< QList<ExtendedDebugLogEntry *> *>(list->data)->size();
}

ExtendedDebugLogEntry *logEntryAt(QQmlListProperty<ExtendedDebugLogEntry> *list, int index)
{
    return static_cast< QList<ExtendedDebugLogEntry *> *>(list->data)->at(index);
}

void clearLogEntries(QQmlListProperty<ExtendedDebugLogEntry> *list)
{
    return static_cast< QList<ExtendedDebugLogEntry *> *>(list->data)->clear();
}

QQmlListProperty<ExtendedDebugLogEntry> FlightLogManager::logEntries()
{
    return QQmlListProperty<ExtendedDebugLogEntry>(this, &m_logEntries, &addLogEntries, &countLogEntries, &logEntryAt, &clearLogEntries);
}

QStringList FlightLogManager::flightEntries()
{
    return m_flightEntries;
}

void FlightLogManager::clearAllLogs()
{
    setDisableControls(true);
    QApplication::setOverrideCursor(Qt::WaitCursor);

    // Clear on flight side
    UAVObjectUpdaterHelper updateHelper;

    m_flightLogControl->setFlight(0);
    m_flightLogControl->setEntry(0);
    m_flightLogControl->setOperation(DebugLogControl::OPERATION_FORMATFLASH);
    if (updateHelper.doObjectAndWait(m_flightLogControl, UAVTALK_TIMEOUT) == UAVObjectUpdaterHelper::SUCCESS) {
        // Then empty locally
        clearLogList();
    }

    QApplication::restoreOverrideCursor();
    setDisableControls(false);
}

void FlightLogManager::clearLogList()
{
    QList<ExtendedDebugLogEntry*> tmpList(m_logEntries);
    m_logEntries.clear();

    emit logEntriesChanged();

    while (!tmpList.isEmpty()) {
        delete tmpList.takeFirst();
    }
}

void FlightLogManager::retrieveLogs(int flightToRetrieve)
{
    setDisableControls(true);
    QApplication::setOverrideCursor(Qt::WaitCursor);

    UAVObjectUpdaterHelper updateHelper;
    UAVObjectRequestHelper requestHelper;

    clearLogList();

    // Set up what to retrieve
    int startFlight = (flightToRetrieve == -1) ? 0 : flightToRetrieve;
    int endFlight   = (flightToRetrieve == -1) ? m_flightLogStatus->getFlight() : flightToRetrieve;

    // Prepare to send request for event retrieval
    m_flightLogControl->setOperation(DebugLogControl::OPERATION_RETRIEVE);
    for (int flight = startFlight; flight <= endFlight; flight++) {
        m_flightLogControl->setFlight(flight);
        bool gotLast = false;
        int entry    = 0;
        while (!gotLast) {
            // Send request for loading flight entry on flight side and wait for ack/nack
            m_flightLogControl->setEntry(entry);

            if (updateHelper.doObjectAndWait(m_flightLogControl, UAVTALK_TIMEOUT) == UAVObjectUpdaterHelper::SUCCESS &&
                requestHelper.doObjectAndWait(m_flightLogEntry, UAVTALK_TIMEOUT) == UAVObjectUpdaterHelper::SUCCESS) {
                if (m_flightLogEntry->getType() != DebugLogEntry::TYPE_EMPTY) {
                    // Ok, we retrieved the entry, and it was the correct one. clone it and add it to the list
                    ExtendedDebugLogEntry *logEntry = new ExtendedDebugLogEntry();
                    logEntry->setObjectManager(m_objectManager);
                    logEntry->setData(m_flightLogEntry->getData());
                    m_logEntries << logEntry;

                    // Increment to get next entry from flight side
                    entry++;
                } else {
                    // We are done, not more entries on this flight
                    gotLast = true;
                }
            } else {
                // We failed for some reason
                break;
            }
        }
    }
    emit logEntriesChanged();
    QApplication::restoreOverrideCursor();
    setDisableControls(false);
}

void FlightLogManager::exportLogs()
{}

void FlightLogManager::updateFlightEntries(quint16 currentFlight)
{
    Q_UNUSED(currentFlight);

    int flights = m_flightLogStatus->getFlight();
    if (m_flightEntries.count() == 0 || (m_flightEntries.count() - 1 != flights)) {
        m_flightEntries.clear();

        m_flightEntries << tr("All");
        for(int i = 0; i <= flights; i++) {
            m_flightEntries << QString::number(i + 1);
        }

        emit flightEntriesChanged();
    }
}

ExtendedDebugLogEntry::ExtendedDebugLogEntry() : DebugLogEntry(),
    m_objectManager(0), m_object(0)
{}

ExtendedDebugLogEntry::~ExtendedDebugLogEntry()
{
    if (m_object) {
        delete m_object;
        m_object = 0;
    }
}

QString ExtendedDebugLogEntry::getLogString()
{
    if (getType() == DebugLogEntry::TYPE_TEXT) {
        return QString((const char *)getData().Data);
    } else if (getType() == DebugLogEntry::TYPE_UAVOBJECT) {
        UAVDataObject *object = (UAVDataObject *)m_objectManager->getObject(getObjectID(), getInstanceID());
        object   = object->clone(getInstanceID());
        object->unpack(getData().Data);
        m_object = object;
        return object->toString().replace("\n", " ").replace("\t", " ");
    } else {
        return "";
    }
}

void ExtendedDebugLogEntry::setObjectManager(UAVObjectManager *objectManager)
{
    m_objectManager = objectManager;
}