#pragma once
/**
 * @file   ProbeComm.hpp
 * @brief  Communication handler for probe data acquisition and event transmission.
 * @details ProbeComm manages all SOME/IP service discovery, event data reception,
 *          payload construction, segmentation, and transmission to DAQ and GEDR targets.
 *          It also handles CameraHost image acquisition, trigger acceptance reporting to ZAT,
 *          and development log file output. Thread-safe for concurrent event processing.
 * @author  Engineering Team
 * @date    2024-01-01
 * @version 1.0.0
 * @copyright Copyright (c) 2024 Company. All rights reserved.
 */

#include <cstdint>
#include <cstddef>
#include <vector>
#include <map>
#include <string>
#include <mutex>
#include <atomic>
#include <memory>
#include <optional>

/**
 * @defgroup ProbeCommModule Probe Communication Components
 * @brief Components responsible for probe data communication with DAQ, GEDR, ZAT, and CameraHost.
 */

/// @namespace probe
/// @brief Contains all components of the probe application.
namespace probe {

/**
 * @class ProbeComm
 * @brief Manages probe communication for data acquisition and event transmission.
 * @details ProbeComm is the central communication handler responsible for:
 *          - SOME/IP service discovery for DAQ, ZAT, CameraHost, TSR, HDF, SDMAP services
 *          - Offering probe event status and trigger services
 *          - Receiving and processing trigger events, camera images, and bus data
 *          - Constructing, segmenting, and transmitting event payloads to DAQ and GEDR
 *          - Managing retry logic, timeout enforcement, and data lifecycle
 *          - Reporting trigger acceptance/rejection status to ZAT
 *          - Optional development log file output
 *          Thread-safety is ensured via std::mutex for shared data access.
 *          Lifecycle: constructed at startup, services discovered, then cyclic/event processing.
 * @ingroup ProbeCommModule
 * @note Compliant with MISRA C++ guidelines, ISO 26262 ASIL-B, C++14/C++17 standard.
 * @warning All service discovery methods must be called before cyclic data processing begins.
 *          Calling transmission methods before DAQ communication is established will suppress data.
 * @invariant sendBufferMutex_ must be held when accessing shared transmission buffers.
 *            cameraMutex_ must be held when accessing image reception state.
 * @see ProbeCommVariant
 */
class ProbeComm
{
public:

    /* ===================================================================
     * Construction / Destruction
     * =================================================================== */

    /**
     * @brief Constructs ProbeComm with variant reference for communication setup.
     * @details Initializes the ProbeComm communication layer by storing the variant
     *          configuration and setting all internal state to default values.
     *          Must be called once during application startup before any service discovery.
     * @param[in] variant Non-owning pointer to the ProbeComm variant configuration.
     *                    Must not be nullptr. Lifetime managed by caller. Valid for
     *                    the entire lifetime of this ProbeComm instance.
     * @throws std::invalid_argument If variant is nullptr.
     * @pre variant points to a fully initialized ProbeCommVariant object.
     * @post All internal members are initialized to safe default values.
     * @note Called by: ProbeApp
     * @warning variant must remain valid for the lifetime of this object.
     * @see ProbeCommVariant
     * @requirements SWR-REQ-01-04-001;SWR-REQ-03-16-002
     * @rationale Variant-based initialization allows flexible configuration per vehicle platform.
     */
    explicit ProbeComm(void* variant);

    /**
     * @brief Destructs ProbeComm and releases all resources.
     * @details Ensures all mutexes are released and any pending operations are safely terminated.
     * @throws None
     * @pre No other thread is actively calling methods on this instance.
     * @post All resources are released.
     * @note Destructor is non-virtual as this class is not intended for polymorphic use.
     */
    ~ProbeComm() noexcept;

    /** @brief Deleted copy constructor for RAII compliance. */
    ProbeComm(const ProbeComm&) = delete;

    /** @brief Deleted copy assignment operator for RAII compliance. */
    ProbeComm& operator=(const ProbeComm&) = delete;

    /** @brief Deleted move constructor for safety. */
    ProbeComm(ProbeComm&&) = delete;

    /** @brief Deleted move assignment operator for safety. */
    ProbeComm& operator=(ProbeComm&&) = delete;

    /* ===================================================================
     * Service Discovery — Find Services
     * =================================================================== */

    /**
     * @brief Locates the DAQ SOME/IP continual data service for steady-state data transmission.
     * @details Discovers the DAQ continual data service endpoint using service discovery.
     *          Sets internal state to allow subsequent steady-state data transmission.
     * @throws std::runtime_error If service discovery fails after internal timeout.
     * @pre ProbeComm has been constructed with a valid variant.
     * @post daqCommunicationEstablished_ may be set to true upon successful discovery.
     * @note Called by: ProbeApp at startup.
     * @warning Blocking call; must complete before cyclic transmission begins.
     * @see SuppressDataBeforeDaqEstablished
     * @requirements SWR-REQ-01-04-001;SWR-REQ-01-06-003;SWR-REQ-03-16-002
     * @rationale Service discovery is required before any DAQ communication can proceed.
     */
    void FindServiceDaqContinual();

    /**
     * @brief Locates the DAQ SOME/IP development event data service for BDP event transmission.
     * @details Discovers the DAQ development event service endpoint for BDP protocol communication.
     * @throws std::runtime_error If service discovery fails.
     * @pre ProbeComm has been constructed with a valid variant.
     * @post DAQ event service proxy is available for BDP operations.
     * @note Called by: ProbeApp at startup.
     * @warning Blocking call; must complete before event data transmission begins.
     * @see SendBdpUploadRequestToDAQ, TransmitBdpDataToDAQ
     * @requirements SWR-REQ-01-04-001;SWR-REQ-03-16-002
     * @rationale BDP event service must be discovered separately from continual service.
     */
    void FindServiceDaqDevEvent();

    /**
     * @brief Locates the ZAT trigger service for receiving event triggers at 100ms periodic rate.
     * @details Discovers the ZAT trigger service and subscribes to trigger events.
     * @throws std::runtime_error If service discovery fails.
     * @pre ProbeComm has been constructed with a valid variant.
     * @post ZAT trigger event subscription is active.
     * @note Called by: ProbeApp at startup.
     * @warning Must be called before HandleAdasAcoreTriggerEvt can receive data.
     * @see HandleAdasAcoreTriggerEvt
     * @requirements SWR-REQ-03-02-001;SWR-REQ-03-003
     * @rationale ZAT provides the primary trigger source for probe event data collection.
     */
    void FindServiceZatTrigger();

    /**
     * @brief Locates ZAT regular upload data service for receiving regular upload data.
     * @details Discovers the ZAT regular upload service and subscribes to data events.
     * @throws std::runtime_error If service discovery fails.
     * @pre ProbeComm has been constructed with a valid variant.
     * @post ZAT regular upload event subscription is active.
     * @note Called by: ProbeApp at startup.
     * @warning Must be called before HandleZatRegularUploadData1Evt can receive data.
     * @see HandleZatRegularUploadData1Evt
     * @requirements SWR-REQ-01-01-001
     * @rationale Regular upload data is needed for steady-state signal collection.
     */
    void FindServiceZatRegularUpload();

    /**
     * @brief Locates the CameraHost image service for receiving camera images.
     * @details Discovers the CameraHost image service and subscribes to image events.
     * @throws std::runtime_error If service discovery fails.
     * @pre ProbeComm has been constructed with a valid variant.
     * @post CameraHost image event subscription is active.
     * @note Called by: ProbeApp at startup.
     * @warning Must be called before image acquisition can proceed.
     * @see HandleCam2ProbePictureEvt, ReceiveCameraImages
     * @requirements SWR-REQ-03-06-002;SWR-REQ-03-06-006
     * @rationale Camera images are a key component of event data payloads.
     */
    void FindServiceCameraImg();

    /**
     * @brief Locates the CameraHost A-HDF/G-HDF data service for receiving camera object data.
     * @details Discovers the CameraHost AGHDF service and subscribes to object data events.
     * @throws std::runtime_error If service discovery fails.
     * @pre ProbeComm has been constructed with a valid variant.
     * @post CameraHost AGHDF event subscription is active.
     * @note Called by: ProbeApp at startup.
     * @warning Must be called before camera object data events can be received.
     * @see HandleCam2ProbeFcCamBusOutEvt
     * @requirements SWR-REQ-03-01-002
     * @rationale Camera object data is required for event log data sets.
     */
    void FindServiceCameraAghdf();

    /**
     * @brief Locates the TSR2GCoreHDF service for receiving TSR data.
     * @details Discovers the TSR to GCore HDF service and subscribes to TSR data events.
     * @throws std::runtime_error If service discovery fails.
     * @pre ProbeComm has been constructed with a valid variant.
     * @post TSR2GCoreHDF event subscription is active.
     * @note Called by: ProbeApp at startup.
     * @warning Must be called before TSR GCore data events can be received.
     * @see HandleTsr2GCoreHdfEvt
     * @requirements SWR-REQ-03-01-002
     * @rationale TSR GCore data is part of the event signal collection.
     */
    void FindServiceTsr2GCoreHdf();

    /**
     * @brief Locates the TSR2HDF service for receiving TSR ACore data.
     * @details Discovers the TSR to HDF service and subscribes to TSR ACore data events.
     * @throws std::runtime_error If service discovery fails.
     * @pre ProbeComm has been constructed with a valid variant.
     * @post TSR2HDF event subscription is active.
     * @note Called by: ProbeApp at startup.
     * @warning Must be called before TSR ACore data events can be received.
     * @see HandleTsr2HdfEvt
     * @requirements SWR-REQ-03-01-002
     * @rationale TSR ACore data is part of the event signal collection.
     */
    void FindServiceTsr2Hdf();

    /**
     * @brief Locates the hdfAp2CpBusOut service for receiving HDF AP to CP data.
     * @details Discovers the HDF AP to CP bus out service and subscribes to data events.
     * @throws std::runtime_error If service discovery fails.
     * @pre ProbeComm has been constructed with a valid variant.
     * @post hdfAp2CpBusOut event subscription is active.
     * @note Called by: ProbeApp at startup.
     * @warning Must be called before HDF AP to CP data events can be received.
     * @see HandleHdfAp2CpBusOutEvt
     * @requirements SWR-REQ-03-01-002
     * @rationale HDF AP to CP data is part of the event signal collection.
     */
    void FindServiceHdfAp2CpBusOut();

    /**
     * @brief Locates the hdfCp2ApBusOut service for receiving ICCOMMW CP to AP data.
     * @details Discovers the HDF CP to AP bus out service and subscribes to data events.
     * @throws std::runtime_error If service discovery fails.
     * @pre ProbeComm has been constructed with a valid variant.
     * @post hdfCp2ApBusOut event subscription is active.
     * @note Called by: ProbeApp at startup.
     * @warning Must be called before HDF CP to AP data events can be received.
     * @see HandleHdfCp2ApBusOutEvt
     * @requirements SWR-REQ-03-01-002
     * @rationale HDF CP to AP data is part of the event signal collection.
     */
    void FindServiceHdfCp2ApBusOut();

    /**
     * @brief Locates the SDMAP data service for receiving map data.
     * @details Discovers the SDMAP service and subscribes to map data events.
     * @throws std::runtime_error If service discovery fails.
     * @pre ProbeComm has been constructed with a valid variant.
     * @post SDMAP event subscription is active.
     * @note Called by: ProbeApp at startup.
     * @warning Must be called before SDMAP data events can be received.
     * @see HandleSdmapDataEvt
     * @requirements SWR-REQ-03-01-002
     * @rationale SDMAP data is part of the event signal collection.
     */
    void FindServiceSdmap();

    /* ===================================================================
     * Service Discovery — Offer Services
     * =================================================================== */

    /**
     * @brief Offers the probeEventStt service to allow ZAT to subscribe for trigger acceptance status.
     * @details Makes the probe event status service available so that ZAT can subscribe
     *          and receive trigger acceptance/rejection notifications.
     * @throws std::runtime_error If service offering fails.
     * @pre ProbeComm has been constructed with a valid variant.
     * @post probeEventStt service is offered and available for subscription.
     * @note Called by: ProbeApp at startup.
     * @warning Must be called before SendTriggerAcceptanceStatusToZAT.
     * @see SendTriggerAcceptanceStatusToZAT, SendTriggerRejectionToZAT
     * @requirements SWR-REQ-03-05-001;SWR-REQ-03-05-003
     * @rationale ZAT needs trigger acceptance feedback for arbitration decisions.
     */
    void OfferServiceProbeEventStt();

    /**
     * @brief Offers the PROBE_TRG_Service to allow CameraHost to subscribe for image acquisition triggers.
     * @details Makes the probe trigger service available so that CameraHost can subscribe
     *          and receive image acquisition trigger notifications.
     * @throws std::runtime_error If service offering fails.
     * @pre ProbeComm has been constructed with a valid variant.
     * @post PROBE_TRG service is offered and available for subscription.
     * @note Called by: ProbeApp at startup.
     * @warning Must be called before SendImageAcquisitionTriggerToCameraHost.
     * @see SendImageAcquisitionTriggerToCameraHost
     * @requirements SWR-REQ-03-06-001
     * @rationale CameraHost requires trigger notification to begin image capture.
     */
    void OfferServiceProbeTrigger();

    /* ===================================================================
     * Event Handlers — Incoming Data
     * =================================================================== */

    /**
     * @brief Receives and processes development event triggers from ZAT at 100ms periodic cycle.
     * @details Parses the trigger data, validates the upload request flag, checks category
     *          and overlap limits, clamps log times, and initiates event data processing.
     * @param[in] triggerData Raw trigger data bytes from ZAT. Must not be empty.
     *                        Valid range: non-empty vector of uint8_t.
     * @throws std::invalid_argument If triggerData is empty.
     * @pre ZAT trigger service has been discovered via FindServiceZatTrigger.
     * @post zatTriggerReceived_ is set to true; event processing may be initiated.
     * @note Called by: ProbeApp at 100ms cycle.
     * @warning Processing must complete within the 100ms cycle budget.
     * @see FindServiceZatTrigger, ValidateTriggerUploadRequestFlag
     * @requirements SWR-REQ-03-02-001;SWR-REQ-03-02-002;SWR-REQ-03-003
     * @rationale ZAT triggers are the primary event source for probe data collection.
     */
    void HandleAdasAcoreTriggerEvt(const std::vector<uint8_t>& triggerData);

    /**
     * @brief Receives ZAT regular upload data for signal collection.
     * @details Stores the received regular upload data for inclusion in steady-state transmissions.
     * @param[in] data Raw regular upload data bytes from ZAT. Must not be empty.
     *                 Valid range: non-empty vector of uint8_t.
     * @throws std::invalid_argument If data is empty.
     * @pre ZAT regular upload service has been discovered via FindServiceZatRegularUpload.
     * @post Regular upload data is stored for subsequent transmission.
     * @note Called by: ProbeApp at 100ms cycle.
     * @warning Processing must complete within the 100ms cycle budget.
     * @see FindServiceZatRegularUpload
     * @requirements SWR-REQ-01-01-001
     * @rationale Regular upload data provides steady-state signal values.
     */
    void HandleZatRegularUploadData1Evt(const std::vector<uint8_t>& data);

    /**
     * @brief Receives and evaluates the answer back from CameraHost after image acquisition trigger.
     * @details Parses the answer back message and validates acceptance status.
     *          Updates cameraAnswerBackReceived_ state accordingly.
     * @param[in] answerData Raw answer back data bytes from CameraHost. Must not be empty.
     *                       Valid range: non-empty vector of uint8_t.
     * @throws std::invalid_argument If answerData is empty.
     * @pre Image acquisition trigger has been sent via SendImageAcquisitionTriggerToCameraHost.
     * @post cameraAnswerBackReceived_ is updated based on answer back evaluation.
     * @note Called by: ProbeApp on event trigger.
     * @warning Must be received before image transmission phase begins.
     * @see ValidateCameraAnswerBack, SendImageAcquisitionTriggerToCameraHost
     * @requirements SWR-REQ-03-06-002;SWR-REQ-03-06-003;SWR-REQ-03-06-004
     * @rationale Answer back confirms CameraHost readiness for image capture.
     */
    void HandleCam2ProbeAnswerBackEvt(const std::vector<uint8_t>& answerData);

    /**
     * @brief Receives camera images from CameraHost with timestamp and frame counter.
     * @details Stores received JPEG image data, increments receivedImageCount_,
     *          and checks completion against expected 14 images.
     * @param[in] imageData Raw image data bytes from CameraHost. Must not be empty.
     *                      Valid range: non-empty vector of uint8_t containing JPEG data.
     * @throws std::invalid_argument If imageData is empty.
     * @pre CameraHost image service has been discovered and trigger has been sent.
     * @post receivedImageCount_ is incremented; imageAcquisitionComplete_ set when all 14 received.
     * @note Called by: ProbeApp on event trigger.
     * @warning Image data is protected by cameraMutex_.
     * @see ReceiveCameraImages, FindServiceCameraImg
     * @requirements SWR-REQ-03-06-006;SWR-REQ-03-06-007;SWR-REQ-03-06-008;SWR-REQ-03-06-009
     * @rationale Camera images are a critical component of event data payloads.
     */
    void HandleCam2ProbePictureEvt(const std::vector<uint8_t>& imageData);

    /**
     * @brief Receives camera FC object data from CameraHost.
     * @details Stores the FC camera bus output data for event log data set construction.
     * @param[in] data Raw FC camera object data bytes. Must not be empty.
     *                 Valid range: non-empty vector of uint8_t.
     * @throws std::invalid_argument If data is empty.
     * @pre CameraHost AGHDF service has been discovered via FindServiceCameraAghdf.
     * @post FC camera data is stored for log data set construction.
     * @note Called by: ProbeApp on event trigger.
     * @see FindServiceCameraAghdf
     * @requirements SWR-REQ-03-01-002
     * @rationale FC camera object data is part of the event signal collection.
     */
    void HandleCam2ProbeFcCamBusOutEvt(const std::vector<uint8_t>& data);

    /**
     * @brief Receives fused camera object data from CameraHost.
     * @details Stores the fused camera CMBS FCV bus output data for event log data set construction.
     * @param[in] data Raw fused camera object data bytes. Must not be empty.
     *                 Valid range: non-empty vector of uint8_t.
     * @throws std::invalid_argument If data is empty.
     * @pre CameraHost AGHDF service has been discovered via FindServiceCameraAghdf.
     * @post Fused camera data is stored for log data set construction.
     * @note Called by: ProbeApp on event trigger.
     * @see FindServiceCameraAghdf
     * @requirements SWR-REQ-03-01-002
     * @rationale Fused camera object data is part of the event signal collection.
     */
    void HandleCam2ProbeFcCamCmbsFcvBusOutEvt(const std::vector<uint8_t>& data);

    /**
     * @brief Receives lane camera data from CameraHost.
     * @details Stores the lane camera CMBS LN bus output data for event log data set construction.
     * @param[in] data Raw lane camera data bytes. Must not be empty.
     *                 Valid range: non-empty vector of uint8_t.
     * @throws std::invalid_argument If data is empty.
     * @pre CameraHost AGHDF service has been discovered via FindServiceCameraAghdf.
     * @post Lane camera data is stored for log data set construction.
     * @note Called by: ProbeApp on event trigger.
     * @see FindServiceCameraAghdf
     * @requirements SWR-REQ-03-01-002
     * @rationale Lane camera data is part of the event signal collection.
     */
    void HandleCam2ProbeFcCamCmbsLnBusOutEvt(const std::vector<uint8_t>& data);

    /**
     * @brief Receives object camera data from CameraHost.
     * @details Stores the object camera CMBS OBJ bus output data for event log data set construction.
     * @param[in] data Raw object camera data bytes. Must not be empty.
     *                 Valid range: non-empty vector of uint8_t.
     * @throws std::invalid_argument If data is empty.
     * @pre CameraHost AGHDF service has been discovered via FindServiceCameraAghdf.
     * @post Object camera data is stored for log data set construction.
     * @note Called by: ProbeApp on event trigger.
     * @see FindServiceCameraAghdf
     * @requirements SWR-REQ-03-01-002
     * @rationale Object camera data is part of the event signal collection.
     */
    void HandleCam2ProbeFcCamCmbsObjBusOutEvt(const std::vector<uint8_t>& data);

    /**
     * @brief Receives TSR GCore data for event signal collection.
     * @details Stores the TSR GCore HDF data for inclusion in event log data sets.
     * @param[in] data Raw TSR GCore data bytes. Must not be empty.
     *                 Valid range: non-empty vector of uint8_t.
     * @throws std::invalid_argument If data is empty.
     * @pre TSR2GCoreHDF service has been discovered via FindServiceTsr2GCoreHdf.
     * @post TSR GCore data is stored for log data set construction.
     * @note Called by: ProbeApp on event trigger.
     * @see FindServiceTsr2GCoreHdf
     * @requirements SWR-REQ-03-01-002
     * @rationale TSR GCore data is part of the event signal collection.
     */
    void HandleTsr2GCoreHdfEvt(const std::vector<uint8_t>& data);

    /**
     * @brief Receives TSR ACore data for event signal collection.
     * @details Stores the TSR HDF data for inclusion in event log data sets.
     * @param[in] data Raw TSR ACore data bytes. Must not be empty.
     *                 Valid range: non-empty vector of uint8_t.
     * @throws std::invalid_argument If data is empty.
     * @pre TSR2HDF service has been discovered via FindServiceTsr2Hdf.
     * @post TSR ACore data is stored for log data set construction.
     * @note Called by: ProbeApp on event trigger.
     * @see FindServiceTsr2Hdf
     * @requirements SWR-REQ-03-01-002
     * @rationale TSR ACore data is part of the event signal collection.
     */
    void HandleTsr2HdfEvt(const std::vector<uint8_t>& data);

    /**
     * @brief Receives HDF AP to CP bus output data.
     * @details Stores the HDF AP to CP data for inclusion in event log data sets.
     * @param[in] data Raw HDF AP to CP data bytes. Must not be empty.
     *                 Valid range: non-empty vector of uint8_t.
     * @throws std::invalid_argument If data is empty.
     * @pre hdfAp2CpBusOut service has been discovered via FindServiceHdfAp2CpBusOut.
     * @post HDF AP to CP data is stored for log data set construction.
     * @note Called by: ProbeApp on event trigger.
     * @see FindServiceHdfAp2CpBusOut
     * @requirements SWR-REQ-03-01-002
     * @rationale HDF AP to CP data is part of the event signal collection.
     */
    void HandleHdfAp2CpBusOutEvt(const std::vector<uint8_t>& data);

    /**
     * @brief Receives ICCOMMW CP to AP communication data.
     * @details Stores the HDF CP to AP data for inclusion in event log data sets.
     * @param[in] data Raw HDF CP to AP data bytes. Must not be empty.
     *                 Valid range: non-empty vector of uint8_t.
     * @throws std::invalid_argument If data is empty.
     * @pre hdfCp2ApBusOut service has been discovered via FindServiceHdfCp2ApBusOut.
     * @post HDF CP to AP data is stored for log data set construction.
     * @note Called by: ProbeApp on event trigger.
     * @see FindServiceHdfCp2ApBusOut
     * @requirements SWR-REQ-03-01-002
     * @rationale HDF CP to AP data is part of the event signal collection.
     */
    void HandleHdfCp2ApBusOutEvt(const std::vector<uint8_t>& data);

    /**
     * @brief Receives SDMAP map data for event signal collection.
     * @details Stores the SDMAP data for inclusion in event log data sets.
     * @param[in] data Raw SDMAP data bytes. Must not be empty.
     *                 Valid range: non-empty vector of uint8_t.
     * @throws std::invalid_argument If data is empty.
     * @pre SDMAP service has been discovered via FindServiceSdmap.
     * @post SDMAP data is stored for log data set construction.
     * @note Called by: ProbeApp on event trigger.
     * @see FindServiceSdmap
     * @requirements SWR-REQ-03-01-002
     * @rationale SDMAP data is part of the event signal collection.
     */
    void HandleSdmapDataEvt(const std::vector<uint8_t>& data);

    /**
     * @brief Receives and processes the BDP upload completion result code from DAQ.
     * @details Evaluates the result code to determine if event data can be deleted
     *          or if retry/abort actions are needed.
     * @param[in] resultCode BDP upload result code from DAQ. Valid range: [0, 65535].
     * @throws None
     * @pre BDP data has been transmitted to DAQ via TransmitBdpDataToDAQ.
     * @post Event data may be deleted on success, or retry/abort initiated on failure.
     * @note Called by: ProbeApp on event trigger.
     * @see DeleteEventDataAfterDaqSuccess, AbortEventDataTransmissionOnRetryExhaustion
     * @requirements SWR-REQ-03-10-001
     * @rationale DAQ result code determines the lifecycle of event data in memory.
     */
    void HandleResultBdpEvt(uint16_t resultCode);

    /* ===================================================================
     * Steady-State Data Transmission
     * =================================================================== */

    /**
     * @brief Transmits short-cycle steady-state data to DAQ via SOME/IP UDP at 100ms intervals.
     * @details Sends the provided payload as continual short-cycle data. Suppressed if
     *          DAQ communication is not yet established.
     * @param[in] payload Steady-state data payload bytes. Must not be empty.
     *                    Valid range: non-empty vector of uint8_t.
     * @throws std::invalid_argument If payload is empty.
     * @pre DAQ continual service has been discovered. DAQ communication is established.
     * @post Payload is transmitted to DAQ or suppressed if not established.
     * @note Called by: ProbeApp at 100ms cycle.
     * @warning Data is suppressed before DAQ communication is established.
     * @see SuppressDataBeforeDaqEstablished, FindServiceDaqContinual
     * @requirements SWR-REQ-01-001;SWR-REQ-01-06-001;SWR-REQ-01-05-001
     * @rationale Short-cycle data provides high-frequency signal updates to DAQ.
     */
    void SendContinualShortDataCyclic100ms(const std::vector<uint8_t>& payload);

    /**
     * @brief Transmits long-cycle steady-state data to DAQ via SOME/IP UDP at 1000ms intervals.
     * @details Sends the provided payload as continual long-cycle data. Suppressed if
     *          DAQ communication is not yet established.
     * @param[in] payload Steady-state data payload bytes. Must not be empty.
     *                    Valid range: non-empty vector of uint8_t.
     * @throws std::invalid_argument If payload is empty.
     * @pre DAQ continual service has been discovered. DAQ communication is established.
     * @post Payload is transmitted to DAQ or suppressed if not established.
     * @note Called by: ProbeApp at 1000ms cycle.
     * @warning Data is suppressed before DAQ communication is established.
     * @see SuppressDataBeforeDaqEstablished, FindServiceDaqContinual
     * @requirements SWR-REQ-01-002;SWR-REQ-01-06-001;SWR-REQ-01-05-001
     * @rationale Long-cycle data provides low-frequency signal updates to DAQ.
     */
    void SendContinualLongDataCyclic1000ms(const std::vector<uint8_t>& payload);

    /**
     * @brief Sends trigger message to CameraHost when event data generation starts.
     * @details Transmits the ADAS_PROBE2CAM_EVT message containing event information
     *          and event number to CameraHost to initiate image capture.
     * @param[in] eventInfo Event information identifier. Valid range: [0, UINT32_MAX].
     * @param[in] numEvt Number of events. Valid range: [0, 255].
     * @throws std::runtime_error If CameraHost service is not available.
     * @pre PROBE_TRG service has been offered and CameraHost has subscribed.
     * @post Trigger message is sent; awaiting CameraHost answer back.
     * @note Called by: ProbeApp on event trigger.
     * @warning CameraHost must be subscribed before calling this method.
     * @see HandleCam2ProbeAnswerBackEvt, OfferServiceProbeTrigger
     * @requirements SWR-REQ-03-06-001;SWR-REQ-03-18-001
     * @rationale Image acquisition must be triggered at the start of event data generation.
     */
    void SendImageAcquisitionTriggerToCameraHost(uint32_t eventInfo, uint8_t numEvt);

    /**
     * @brief Transmits event data payloads at 1000ms pacing intervals.
     * @details Sends queued event data segments at 1000ms intervals to avoid
     *          overwhelming the DAQ receiver.
     * @throws None
     * @pre Event data has been constructed and segmented.
     * @post One or more event data segments are transmitted per call.
     * @note Called by: ProbeApp at 1000ms cycle.
     * @warning Must respect the transmission order enforced by EnforceEventDataTransmissionOrder.
     * @see EnforceEventDataTransmissionOrder, TransmitBdpDataToDAQ
     * @requirements SWR-REQ-03-07-002;SWR-REQ-03-09-005
     * @rationale 1000ms pacing prevents DAQ receiver buffer overflow.
     */
    void SendEventDataAtCyclic1000ms();

    /**
     * @brief Periodically sends accepted data transmission numbers and category identifiers to ZAT.
     * @details Transmits the ADAS_ACore_PROBE2ZAT_EVT message with current acceptance status.
     * @throws None
     * @pre probeEventStt service has been offered via OfferServiceProbeEventStt.
     * @post Trigger acceptance status is sent to ZAT.
     * @note Called by: ProbeApp at 100ms cycle.
     * @see OfferServiceProbeEventStt, SendTriggerRejectionToZAT
     * @requirements SWR-REQ-03-05-001;SWR-REQ-03-05-004
     * @rationale ZAT needs acceptance feedback for trigger arbitration.
     */
    void SendTriggerAcceptanceStatusToZAT();

    /**
     * @brief Periodically reports the allowed overlap trigger number to trigger arbitration function.
     * @details Transmits the ADAS_ACore_PROBE2ZAT_EVT message with allowed overlap count.
     * @throws None
     * @pre probeEventStt service has been offered via OfferServiceProbeEventStt.
     * @post Allowed overlap count is sent to ZAT.
     * @note Called by: ProbeApp at 100ms cycle.
     * @see OfferServiceProbeEventStt
     * @requirements SWR-REQ-03-05-003;SWR-REQ-03-04-004
     * @rationale ZAT uses overlap count for trigger arbitration decisions.
     */
    void SendAllowedOverlapCountToZAT();

    /**
     * @brief Resets all per-category acceptance counters to zero when a new driving cycle begins.
     * @details Clears the categoryTriggerCountMap_ to allow fresh trigger acceptance counting.
     * @throws None
     * @pre None.
     * @post All per-category counters are reset to zero.
     * @note Called by: ProbeApp at startup.
     * @see CheckSameCategoryLimit
     * @requirements SWR-REQ-03-12-004
     * @rationale Per-category limits are per driving cycle and must be reset on new cycle.
     */
    void ResetCategoryCounterOnNewDriveCycle() noexcept;

    /**
     * @brief Enables DAQ data logging in development builds only.
     * @details Sets devLogEnabled_ to true, enabling WriteDevLogSentData and WriteDevLogReceivedData.
     * @throws None
     * @pre None.
     * @post devLogEnabled_ is true.
     * @note Called by: ProbeApp at startup.
     * @warning Must only be enabled in development builds, not production.
     * @see DisableDevLogFileOutput, WriteDevLogSentData, WriteDevLogReceivedData
     * @requirements SWR-REQ-01-11-001;SWR-REQ-03-15-001
     * @rationale Development logging aids debugging but must be disabled in production.
     */
    void EnableDevLogFileOutput() noexcept;

    /**
     * @brief Disables DAQ data logging for production builds.
     * @details Sets devLogEnabled_ to false, disabling all development log file output.
     * @throws None
     * @pre None.
     * @post devLogEnabled_ is false.
     * @note Called by: ProbeApp at startup.
     * @see EnableDevLogFileOutput
     * @requirements SWR-REQ-01-11-003;SWR-REQ-03-15-003
     * @rationale Production builds must not produce development log files.
     */
    void DisableDevLogFileOutput() noexcept;

private:

    /* ===================================================================
     * Private Methods — Validation
     * =================================================================== */

    /**
     * @brief Checks if the data upload request flag is non-zero to determine valid trigger.
     * @details Returns true if the flag value is non-zero, indicating a valid upload request.
     * @param[in] flag Data upload request flag value. Valid range: [0, 255].
     * @return True if the flag is non-zero (valid trigger), false otherwise.
     * @retval true  Upload request flag is non-zero — valid trigger.
     * @retval false Upload request flag is zero — no valid trigger.
     * @throws None
     * @pre None.
     * @post No state change.
     * @note Called by: ProbeComm internally during trigger processing.
     * @requirements SWR-REQ-03-02-002;SWR-REQ-03-003
     * @rationale Zero flag indicates no upload request; non-zero indicates valid trigger.
     */
    bool ValidateTriggerUploadRequestFlag(uint8_t flag) const noexcept;

    /**
     * @brief Validates whether the acceptance counter for a category has reached the configurable limit.
     * @details Looks up the category in categoryTriggerCountMap_ and compares against sameCategoryLimitPerDC_.
     * @param[in] categoryId Category identifier to check. Valid range: [0, UINT32_MAX].
     * @return True if the category limit has NOT been reached (acceptance allowed), false if limit reached.
     * @retval true  Category acceptance count is below the limit.
     * @retval false Category acceptance count has reached or exceeded the limit.
     * @throws None
     * @pre categoryTriggerCountMap_ is initialized.
     * @post No state change.
     * @note Called by: ProbeComm internally during trigger validation.
     * @requirements SWR-REQ-03-12-001;SWR-REQ-03-12-002;SWR-REQ-03-12-005;SWR-REQ-03-12-006
     * @rationale Per-category limits prevent excessive data collection for a single category per drive cycle.
     */
    bool CheckSameCategoryLimit(uint32_t categoryId) const noexcept;

    /**
     * @brief Validates whether the active event process count exceeds the allowed overlap limit.
     * @details Compares activeEventProcessCount_ against allowedOverlapTriggerNum_.
     * @return True if overlap limit has NOT been exceeded (new trigger allowed), false otherwise.
     * @retval true  Active event count is below the overlap limit.
     * @retval false Active event count has reached or exceeded the overlap limit.
     * @throws None
     * @pre None.
     * @post No state change.
     * @note Called by: ProbeComm internally during trigger validation.
     * @requirements SWR-REQ-03-04-003;SWR-REQ-03-04-004
     * @rationale Overlap limit prevents resource exhaustion from too many concurrent events.
     */
    bool CheckOverlapTriggerLimit() const noexcept;

    /**
     * @brief Clamps log data start time to the negative of past data retention setting when exceeded.
     * @details If startTime is less than the negative of retentionLimit, returns -retentionLimit.
     *          Otherwise returns startTime unchanged.
     * @param[in] startTime Log data start time in seconds. Valid range: [INT32_MIN, 0].
     * @param[in] retentionLimit Past data retention limit in seconds. Valid range: [0, UINT32_MAX].
     * @return Clamped start time value.
     * @retval Clamped value if startTime exceeds retention limit.
     * @retval Original startTime if within retention limit.
     * @throws None
     * @pre None.
     * @post No state change.
     * @note Called by: ProbeComm internally during trigger processing.
     * @requirements SWR-REQ-03-02-003;SWR-REQ-03-17-007
     * @rationale Clamping prevents requesting data beyond the available retention window.
     */
    int32_t ValidateLogStartTimeClamping(int32_t startTime, uint32_t retentionLimit) const noexcept;

    /**
     * @brief Clamps log data end time to +83 when the received value exceeds the limit.
     * @details If endTime exceeds +83 seconds, returns 83. Otherwise returns endTime unchanged.
     * @param[in] endTime Log data end time in seconds. Valid range: [INT32_MIN, INT32_MAX].
     * @return Clamped end time value.
     * @retval 83 if endTime exceeds +83 seconds.
     * @retval Original endTime if within the +83 limit.
     * @throws None
     * @pre None.
     * @post No state change.
     * @note Called by: ProbeComm internally during trigger processing.
     * @requirements SWR-REQ-03-02-004
     * @rationale +83 seconds is the maximum future data window for event log data.
     */
    int32_t ValidateLogEndTimeClamping(int32_t endTime) const noexcept;

    /* ===================================================================
     * Private Methods — DAQ Communication
     * =================================================================== */

    /**
     * @brief Transmits DAQ_UNIVERSAL_ETH_DATA_FIR_FGT message with client ID, PDU ID, and data vector.
     * @details Sends a fire-and-forget UDP message to DAQ with the specified parameters.
     *          Protected by sendBufferMutex_.
     * @param[in] clientId Client identifier. Valid range: [0, 65535].
     * @param[in] pduId PDU identifier. Valid range: [0, UINT32_MAX].
     * @param[in] dataVector Data payload bytes. Must not be empty.
     * @throws std::runtime_error If DAQ communication is not established.
     * @pre DAQ continual service has been discovered and communication is established.
     * @post Message is sent to DAQ via UDP.
     * @note Called by: ProbeComm internally.
     * @warning Protected by sendBufferMutex_ for thread safety.
     * @see SuppressDataBeforeDaqEstablished
     * @requirements SWR-REQ-01-005;SWR-REQ-01-06-002;SWR-REQ-01-05-002
     * @rationale Fire-and-forget is used for non-critical steady-state data.
     */
    void SendDaqFireForgetMethod(uint16_t clientId, uint32_t pduId,
                                 const std::vector<uint8_t>& dataVector);

    /**
     * @brief Returns true to suppress data transmission when SOME/IP communication with DAQ is not yet established.
     * @details Checks daqCommunicationEstablished_ flag.
     * @return True if data should be suppressed (DAQ not established), false if transmission is allowed.
     * @retval true  DAQ communication is NOT established — suppress data.
     * @retval false DAQ communication IS established — allow transmission.
     * @throws None
     * @pre None.
     * @post No state change.
     * @note Called by: ProbeComm internally before any DAQ transmission.
     * @requirements SWR-REQ-01-04-001;SWR-REQ-01-04-002
     * @rationale Data must not be sent before the communication channel is ready.
     */
    bool SuppressDataBeforeDaqEstablished() const noexcept;

    /* ===================================================================
     * Private Methods — Camera Image Handling
     * =================================================================== */

    /**
     * @brief Receives 14 JPEG images covering -5 to +8 seconds around the event with timeout enforcement.
     * @details Manages the image reception state machine, tracking received images
     *          and enforcing the configurable timeout.
     * @throws None
     * @pre Image acquisition trigger has been sent and CameraHost has accepted.
     * @post imageAcquisitionComplete_ is set when all 14 images are received or timeout occurs.
     * @note Called by: ProbeComm internally during event processing.
     * @warning Protected by cameraMutex_ for thread safety.
     * @see EnforceImageAcquisitionTimeout, HandleCam2ProbePictureEvt
     * @requirements SWR-REQ-03-06-006;SWR-REQ-03-06-010
     * @rationale 14 images at 1-second intervals cover the required time window around the event.
     */
    void ReceiveCameraImages();

    /**
     * @brief Evaluates acceptance status from CameraHost answer back message.
     * @details Checks the answer back byte to determine if CameraHost accepted the trigger.
     * @param[in] answerBack Answer back status byte from CameraHost. Valid range: [0, 255].
     * @return True if CameraHost accepted the trigger, false if rejected.
     * @retval true  CameraHost accepted the image acquisition trigger.
     * @retval false CameraHost rejected the image acquisition trigger.
     * @throws None
     * @pre Answer back data has been received from CameraHost.
     * @post No state change.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-06-002;SWR-REQ-03-06-003
     * @rationale Acceptance validation determines whether to proceed with image reception.
     */
    bool ValidateCameraAnswerBack(uint8_t answerBack) const noexcept;

    /**
     * @brief Processes trigger-stage and per-image error codes from CameraHost.
     * @details Logs and handles error codes received during image acquisition.
     * @param[in] errorCode Error code from CameraHost. Valid range: [0, 65535].
     * @throws None
     * @pre Error code has been received from CameraHost.
     * @post Error state is updated internally.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-06-004;SWR-REQ-03-06-005
     * @rationale Error codes indicate specific failure modes in image acquisition.
     */
    void HandleCameraErrorCode(uint16_t errorCode);

    /**
     * @brief Enforces configurable 55-second timeout for image acquisition from CameraHost.
     * @details Checks if the elapsed time since image acquisition start exceeds the timeout.
     * @return True if timeout has NOT expired (still within time), false if timeout expired.
     * @retval true  Image acquisition is still within the timeout window.
     * @retval false Image acquisition timeout has expired.
     * @throws None
     * @pre Image acquisition has been initiated.
     * @post No state change.
     * @note Called by: ProbeComm internally.
     * @warning Protected by cameraMutex_.
     * @requirements SWR-REQ-03-06-010
     * @rationale Timeout prevents indefinite waiting for camera images.
     */
    bool EnforceImageAcquisitionTimeout() const noexcept;

    /**
     * @brief Abandons image acquisition and image transmission processing when timeout expires.
     * @details Resets image reception state and marks acquisition as failed.
     * @throws None
     * @pre Image acquisition timeout has expired.
     * @post imageAcquisitionComplete_ is set to true with failure indication.
     * @note Called by: ProbeComm internally.
     * @warning Protected by cameraMutex_.
     * @see EnforceImageAcquisitionTimeout
     * @requirements SWR-REQ-03-06-010
     * @rationale Timeout abort prevents blocking event data transmission indefinitely.
     */
    void AbortImageAcquisitionOnTimeout();

    /**
     * @brief Abandons image acquisition if answer back not received before image transmission phase.
     * @details Checks cameraAnswerBackReceived_ and aborts if not set.
     * @throws None
     * @pre Image acquisition trigger has been sent.
     * @post Image processing is aborted if answer back was not received.
     * @note Called by: ProbeComm internally.
     * @see ValidateCameraAnswerBack
     * @requirements SWR-REQ-03-06-003
     * @rationale Missing answer back indicates CameraHost did not acknowledge the trigger.
     */
    void AbortImageProcessingBeforeTransmitPhase();

    /**
     * @brief Rounds up PictureSize to multiple of 4 bytes for payload assembly.
     * @details Creates a new vector with size rounded up to the nearest 4-byte boundary,
     *          padding with zeros if necessary.
     * @param[in] pictureSize Original picture size in bytes. Valid range: [0, 65535].
     * @param[in] imageData Raw image data bytes.
     * @return Aligned image data vector with size as a multiple of 4.
     * @throws std::invalid_argument If pictureSize does not match imageData size.
     * @pre imageData contains valid JPEG image data.
     * @post Returned vector size is a multiple of 4.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-06-007
     * @rationale 4-byte alignment is required for payload assembly protocol compliance.
     */
    std::vector<uint8_t> CopyImageDataAligned4Bytes(uint16_t pictureSize,
                                                     const std::vector<uint8_t>& imageData);

    /* ===================================================================
     * Private Methods — Payload Construction
     * =================================================================== */

    /**
     * @brief Builds header with timestamp, category optional data, and event list payload.
     * @details Constructs the event data type 001 header structure.
     * @param[in] categoryId Category identifier. Valid range: [0, UINT32_MAX].
     * @param[in] transNum Transmission number. Valid range: [0, 255].
     * @return Serialized header bytes for event data type 001.
     * @throws None
     * @pre categoryId and transNum are valid.
     * @post Returned vector contains the complete type 001 header.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-07-003;SWR-REQ-03-07-010
     * @rationale Type 001 header is the first payload in the event data transmission sequence.
     */
    std::vector<uint8_t> FillHeaderEventDataType001(uint32_t categoryId, uint8_t transNum);

    /**
     * @brief Builds header with timestamp and log data set payload.
     * @details Constructs the event data type 002 header structure.
     * @param[in] logDataSet Serialized log data set bytes. Must not be empty.
     * @return Serialized header bytes for event data type 002.
     * @throws std::invalid_argument If logDataSet is empty.
     * @pre logDataSet contains valid serialized log data.
     * @post Returned vector contains the complete type 002 header.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-07-004
     * @rationale Type 002 header wraps the log data set for transmission.
     */
    std::vector<uint8_t> FillHeaderEventDataType002(const std::vector<uint8_t>& logDataSet);

    /**
     * @brief Builds header with timestamp and 14 image frames payload.
     * @details Constructs the event data type 003 header structure.
     * @param[in] imageData Serialized image data bytes. Must not be empty.
     * @return Serialized header bytes for event data type 003.
     * @throws std::invalid_argument If imageData is empty.
     * @pre imageData contains valid serialized image frames.
     * @post Returned vector contains the complete type 003 header.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-07-005
     * @rationale Type 003 header wraps the image data set for transmission.
     */
    std::vector<uint8_t> FillHeaderEventDataType003(const std::vector<uint8_t>& imageData);

    /**
     * @brief Constructs the first event payload with category data and event list.
     * @details Builds the category optional data and event list from trigger data.
     * @param[in] triggerData Raw trigger data bytes from ZAT. Must not be empty.
     * @return Serialized payload bytes containing category optional data and event list.
     * @throws std::invalid_argument If triggerData is empty.
     * @pre triggerData contains valid trigger information.
     * @post Returned vector contains the complete category and event list payload.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-07-001;SWR-REQ-03-07-003
     * @rationale Category and event list payload is the first data type in event transmission.
     */
    std::vector<uint8_t> BuildPayloadCategoryOptionalAndEventList(
        const std::vector<uint8_t>& triggerData);

    /**
     * @brief Constructs the second event payload with log data set in chronological order.
     * @details Assembles log data entries in time-ordered sequence.
     * @param[in] logData Raw log data bytes. Must not be empty.
     * @return Serialized payload bytes containing the chronologically ordered log data set.
     * @throws std::invalid_argument If logData is empty.
     * @pre logData contains valid log entries.
     * @post Returned vector contains the complete log data set payload.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-07-004;SWR-REQ-03-07-006
     * @rationale Log data must be in chronological order per protocol specification.
     */
    std::vector<uint8_t> BuildPayloadLogDataSet(const std::vector<uint8_t>& logData);

    /**
     * @brief Constructs the third event payload with 14 image frames and timestamps.
     * @details Assembles 14 JPEG image frames with their associated timestamps.
     * @param[in] imageData Raw image data bytes. Must not be empty.
     * @return Serialized payload bytes containing the 14 image frames with timestamps.
     * @throws std::invalid_argument If imageData is empty.
     * @pre imageData contains valid image frame data.
     * @post Returned vector contains the complete image data set payload.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-07-005
     * @rationale Image data set is the third and final data type in event transmission.
     */
    std::vector<uint8_t> BuildPayloadImageDataSet(const std::vector<uint8_t>& imageData);

    /* ===================================================================
     * Private Methods — Segmentation
     * =================================================================== */

    /**
     * @brief Divides event data into numbered segments when total size exceeds max packet size.
     * @details Splits the payload into segments with sequential numbering and position indicators.
     * @param[in] payload Complete event data payload bytes. Must not be empty.
     * @return Vector of segment vectors, each within the maximum packet size.
     * @throws std::invalid_argument If payload is empty.
     * @pre payload contains valid event data.
     * @post Each returned segment is within the maximum allowed packet size.
     * @note Called by: ProbeComm internally.
     * @see SetDataInformationFlag
     * @requirements SWR-REQ-03-08-001;SWR-REQ-03-08-002;SWR-REQ-03-08-003;SWR-REQ-03-07-008
     * @rationale Large payloads must be segmented to fit within SOME/IP packet size limits.
     */
    std::vector<std::vector<uint8_t>> SegmentPayloadIfExceedsMaxSize(
        const std::vector<uint8_t>& payload);

    /**
     * @brief Marks each segment with start, middle, end, or single-packet indicator.
     * @details Computes the data information flag byte based on segment position.
     * @param[in] isSinglePacket True if the entire payload fits in one packet.
     * @param[in] isStart True if this is the first segment.
     * @param[in] isEnd True if this is the last segment.
     * @return Data information flag byte.
     * @retval 0x00 Single packet (isSinglePacket is true).
     * @retval 0x01 Start segment.
     * @retval 0x02 Middle segment.
     * @retval 0x03 End segment.
     * @throws None
     * @pre Exactly one valid combination of flags is provided.
     * @post No state change.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-08-002;SWR-REQ-03-08-003
     * @rationale Position indicators allow the receiver to reassemble segmented payloads.
     */
    uint8_t SetDataInformationFlag(bool isSinglePacket, bool isStart, bool isEnd) const noexcept;

    /* ===================================================================
     * Private Methods — BDP Protocol
     * =================================================================== */

    /**
     * @brief Sends transmission permission request to DAQ before sending event data.
     * @details Sends the REQUEST_BDP_UPLOAD_REQ_RES message and awaits response.
     * @param[in] clientId Client identifier. Valid range: [0, 65535].
     * @param[in] eventValue Event value identifier. Valid range: [0, UINT32_MAX].
     * @param[in] dataSize Total data size in bytes. Valid range: [0, UINT32_MAX].
     * @param[in] requestNum Request sequence number. Valid range: [0, UINT32_MAX].
     * @return True if DAQ granted permission, false if denied or error.
     * @retval true  DAQ granted upload permission.
     * @retval false DAQ denied permission or communication error occurred.
     * @throws None
     * @pre DAQ dev event service has been discovered.
     * @post Upload permission state is updated.
     * @note Called by: ProbeComm internally.
     * @see HandleBdpRequestResponseStatusCode, EnforceBdpRequestRetry
     * @requirements SWR-REQ-03-09-001;SWR-REQ-03-09-002
     * @rationale Permission request prevents data loss from unsolicited transmissions.
     */
    bool SendBdpUploadRequestToDAQ(uint16_t clientId, uint32_t eventValue,
                                    uint32_t dataSize, uint32_t requestNum);

    /**
     * @brief Transmits event data payload to DAQ via SOME/IP TCP BDP protocol.
     * @details Sends the TRANSMIT_BDP_REQ_RES message with the event data contents.
     * @param[in] clientId Client identifier. Valid range: [0, 65535].
     * @param[in] requestNum Request sequence number. Valid range: [0, UINT32_MAX].
     * @param[in] contents Event data payload bytes. Must not be empty.
     * @return True if DAQ acknowledged receipt, false if error.
     * @retval true  DAQ acknowledged successful receipt of data.
     * @retval false DAQ reported error or communication failure.
     * @throws None
     * @pre Upload permission has been granted by DAQ.
     * @post Data is transmitted to DAQ.
     * @note Called by: ProbeComm internally.
     * @see HandleBdpTransmitReplyStatusCode, EnforceBdpTransmitRetry
     * @requirements SWR-REQ-03-07-011;SWR-REQ-03-09-002
     * @rationale BDP TCP protocol ensures reliable event data delivery.
     */
    bool TransmitBdpDataToDAQ(uint16_t clientId, uint32_t requestNum,
                               const std::vector<uint8_t>& contents);

    /**
     * @brief Evaluates DAQ response to upload request and triggers retry or proceed.
     * @details Parses the status code and determines the next action.
     * @param[in] statusCode DAQ response status code. Valid range: [0, 65535].
     * @return True if request was accepted, false if retry or abort is needed.
     * @retval true  Upload request was accepted by DAQ.
     * @retval false Upload request was rejected — retry or abort required.
     * @throws None
     * @pre BDP upload request has been sent.
     * @post Retry or proceed action is determined.
     * @note Called by: ProbeComm internally.
     * @see EnforceBdpRequestRetry
     * @requirements SWR-REQ-03-09-002;SWR-REQ-03-09-003
     * @rationale Status code evaluation drives the BDP state machine.
     */
    bool HandleBdpRequestResponseStatusCode(uint16_t statusCode);

    /**
     * @brief Evaluates DAQ reply after data transmission.
     * @details Parses the transmit reply status code and determines the next action.
     * @param[in] statusCode DAQ transmit reply status code. Valid range: [0, 65535].
     * @return True if transmission was acknowledged, false if retry or abort is needed.
     * @retval true  Data transmission was acknowledged by DAQ.
     * @retval false Data transmission failed — retry or abort required.
     * @throws None
     * @pre BDP data has been transmitted.
     * @post Retry or proceed action is determined.
     * @note Called by: ProbeComm internally.
     * @see EnforceBdpTransmitRetry
     * @requirements SWR-REQ-03-09-002;SWR-REQ-03-11-002
     * @rationale Transmit reply evaluation drives the BDP state machine.
     */
    bool HandleBdpTransmitReplyStatusCode(uint16_t statusCode);

    /**
     * @brief Retries upload request after configurable wait time up to maximum retry count.
     * @details Increments bdpRequestRetryCounter_ and checks against bdpMaxRetryCount_.
     *          Waits bdpRetryIntervalMs_ between retries.
     * @return True if retry is allowed, false if max retries exhausted.
     * @retval true  Retry is allowed — counter incremented and within limit.
     * @retval false Maximum retry count exhausted — abort required.
     * @throws None
     * @pre BDP request was rejected by DAQ.
     * @post bdpRequestRetryCounter_ is incremented.
     * @note Called by: ProbeComm internally.
     * @warning Introduces a blocking wait of bdpRetryIntervalMs_.
     * @see AbortEventDataTransmissionOnRetryExhaustion
     * @requirements SWR-REQ-03-09-003;SWR-REQ-03-11-001
     * @rationale Bounded retry with configurable interval prevents infinite loops.
     */
    bool EnforceBdpRequestRetry();

    /**
     * @brief Retries data transmission after configurable wait time up to maximum retransmission count.
     * @details Increments bdpTransmitRetryCounter_ and checks against bdpMaxRetryCount_.
     *          Waits bdpRetryIntervalMs_ between retries.
     * @return True if retry is allowed, false if max retries exhausted.
     * @retval true  Retry is allowed — counter incremented and within limit.
     * @retval false Maximum retransmission count exhausted — abort required.
     * @throws None
     * @pre BDP transmission was rejected by DAQ.
     * @post bdpTransmitRetryCounter_ is incremented.
     * @note Called by: ProbeComm internally.
     * @warning Introduces a blocking wait of bdpRetryIntervalMs_.
     * @see AbortEventDataTransmissionOnRetryExhaustion
     * @requirements SWR-REQ-03-11-002;SWR-REQ-03-11-005
     * @rationale Bounded retry with configurable interval prevents infinite loops.
     */
    bool EnforceBdpTransmitRetry();

    /**
     * @brief Resets all retry counters to zero after successful transmission cycle.
     * @details Clears bdpRequestRetryCounter_, bdpTransmitRetryCounter_, and gedrWriteRetryCounter_.
     * @throws None
     * @pre Transmission cycle completed successfully.
     * @post All retry counters are zero.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-11-006
     * @rationale Counters must be reset for the next event data transmission cycle.
     */
    void ResetRetryCountersOnSuccess() noexcept;

    /**
     * @brief Stops all DAQ transmission and discards data when max retries are exhausted.
     * @details Aborts the current event data transmission and triggers data deletion.
     * @throws None
     * @pre Maximum retry count has been reached for request or transmit.
     * @post All pending event data for this transmission is discarded.
     * @note Called by: ProbeComm internally.
     * @see DeleteEventDataOnRetryExhaustion
     * @requirements SWR-REQ-03-09-004;SWR-REQ-03-11-003;SWR-REQ-03-11-004
     * @rationale Exhausted retries indicate unrecoverable communication failure.
     */
    void AbortEventDataTransmissionOnRetryExhaustion();

    /* ===================================================================
     * Private Methods — Transmission Ordering
     * =================================================================== */

    /**
     * @brief Ensures event data is transmitted in strict order: header+category, log data, image data.
     * @details Manages the transmission state machine to enforce the required ordering.
     * @throws None
     * @pre Event data payloads have been constructed.
     * @post Transmission proceeds in the correct order.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-07-001;SWR-REQ-03-04-002
     * @rationale Protocol requires strict ordering of event data types.
     */
    void EnforceEventDataTransmissionOrder();

    /* ===================================================================
     * Private Methods — DAQ Event Data Transmission
     * =================================================================== */

    /**
     * @brief Transmits header with category optional data and event list to DAQ.
     * @details Sends the type 001 event data payload to DAQ via BDP protocol.
     * @param[in] payload Serialized category data payload bytes. Must not be empty.
     * @throws std::invalid_argument If payload is empty.
     * @pre BDP upload permission has been granted.
     * @post Category data is transmitted to DAQ.
     * @note Called by: ProbeComm internally.
     * @see FillHeaderEventDataType001, BuildPayloadCategoryOptionalAndEventList
     * @requirements SWR-REQ-03-18-002;SWR-REQ-03-07-003
     * @rationale Category data is the first payload in the event transmission sequence.
     */
    void SendAdasCategoryDataToDAQ(const std::vector<uint8_t>& payload);

    /**
     * @brief Transmits log data sets to DAQ in chronological order without waiting for images.
     * @details Sends all log data set payloads to DAQ sequentially.
     * @param[in] logPayloads Vector of serialized log data set payload vectors. Must not be empty.
     * @throws std::invalid_argument If logPayloads is empty.
     * @pre BDP upload permission has been granted and category data has been sent.
     * @post All log data sets are transmitted to DAQ.
     * @note Called by: ProbeComm internally.
     * @see FillHeaderEventDataType002, BuildPayloadLogDataSet
     * @requirements SWR-REQ-03-07-004;SWR-REQ-03-07-006;SWR-REQ-03-07-012;SWR-REQ-03-18-004
     * @rationale Log data is sent independently of image data to avoid blocking.
     */
    void SendLogDataSetsToDAQ(const std::vector<std::vector<uint8_t>>& logPayloads);

    /**
     * @brief Transmits 14 image frames with timestamps to DAQ as soon as image data is received.
     * @details Sends the type 003 event data payload to DAQ via BDP protocol.
     * @param[in] imagePayload Serialized image data payload bytes. Must not be empty.
     * @throws std::invalid_argument If imagePayload is empty.
     * @pre BDP upload permission has been granted and log data has been sent.
     * @post Image data is transmitted to DAQ.
     * @note Called by: ProbeComm internally.
     * @see FillHeaderEventDataType003, BuildPayloadImageDataSet
     * @requirements SWR-REQ-03-07-005;SWR-REQ-03-18-006
     * @rationale Image data is the final payload in the event transmission sequence.
     */
    void SendImageDataSetToDAQ(const std::vector<uint8_t>& imagePayload);

    /* ===================================================================
     * Private Methods — GEDR Transmission
     * =================================================================== */

    /**
     * @brief Transmits category data to GEDR API after DAQ transmission.
     * @details Sends the category data payload to GEDR using UploadGedrWrite.
     * @param[in] payload Serialized category data payload bytes. Must not be empty.
     * @throws std::invalid_argument If payload is empty.
     * @pre DAQ category data transmission is complete.
     * @post Category data is written to GEDR.
     * @note Called by: ProbeComm internally.
     * @see UploadGedrWrite
     * @requirements SWR-REQ-03-17-001;SWR-REQ-03-18-003
     * @rationale GEDR receives category data after DAQ to maintain transmission order.
     */
    void SendAdasCategoryDataToGEDR(const std::vector<uint8_t>& payload);

    /**
     * @brief Transmits log data in 1-second units to GEDR API.
     * @details Sends all log data set payloads to GEDR sequentially using UploadGedrWrite.
     * @param[in] logPayloads Vector of serialized log data set payload vectors. Must not be empty.
     * @throws std::invalid_argument If logPayloads is empty.
     * @pre DAQ log data transmission is complete.
     * @post All log data sets are written to GEDR.
     * @note Called by: ProbeComm internally.
     * @see UploadGedrWrite
     * @requirements SWR-REQ-03-17-002;SWR-REQ-03-18-005
     * @rationale GEDR receives log data in 1-second units per protocol specification.
     */
    void SendLogDataSetsToGEDR(const std::vector<std::vector<uint8_t>>& logPayloads);

    /**
     * @brief Transmits 14 image data set to GEDR API.
     * @details Sends the image data payload to GEDR using UploadGedrWrite.
     * @param[in] imagePayload Serialized image data payload bytes. Must not be empty.
     * @throws std::invalid_argument If imagePayload is empty.
     * @pre DAQ image data transmission is complete.
     * @post Image data is written to GEDR.
     * @note Called by: ProbeComm internally.
     * @see UploadGedrWrite
     * @requirements SWR-REQ-03-17-002;SWR-REQ-03-18-007
     * @rationale GEDR receives image data after DAQ to maintain transmission order.
     */
    void SendImageDataSetToGEDR(const std::vector<uint8_t>& imagePayload);

    /**
     * @brief Calls GEDR_Write with offset, data, data_size, start_block, and end_block parameters.
     * @details Invokes the GEDR API write function with the specified parameters.
     * @param[in] offset Write offset in bytes. Valid range: [0, UINT32_MAX].
     * @param[in] data Data bytes to write. Must not be empty.
     * @param[in] dataSize Size of data in bytes. Valid range: [0, UINT32_MAX].
     * @param[in] startBlock True if this is the start block of a write sequence.
     * @param[in] endBlock True if this is the end block of a write sequence.
     * @return GEDR API return code.
     * @retval 0 Success.
     * @retval Negative value Error code — see HandleGedrRetryOnError and HandleGedrFatalError.
     * @throws None
     * @pre GEDR API is available.
     * @post Data is written to GEDR storage.
     * @note Called by: ProbeComm internally.
     * @see HandleGedrRetryOnError, HandleGedrFatalError
     * @requirements SWR-REQ-03-17-001;SWR-REQ-03-17-003;SWR-REQ-03-17-004;SWR-REQ-03-17-005
     * @rationale Direct GEDR API call with explicit parameters for traceability.
     */
    int8_t UploadGedrWrite(uint32_t offset, const std::vector<uint8_t>& data,
                           uint32_t dataSize, bool startBlock, bool endBlock);

    /**
     * @brief Retries GEDR write on error with bounded retry count.
     * @details Increments gedrWriteRetryCounter_ and checks against gedrMaxRetryCount_.
     * @param[in] returnCode GEDR API return code. Valid range: [INT8_MIN, INT8_MAX].
     * @return True if retry is allowed, false if max retries exhausted.
     * @retval true  Retry is allowed — counter incremented and within limit.
     * @retval false Maximum retry count exhausted — abort GEDR transmission.
     * @throws None
     * @pre GEDR write returned a retryable error.
     * @post gedrWriteRetryCounter_ is incremented.
     * @note Called by: ProbeComm internally.
     * @see HandleGedrFatalError
     * @requirements SWR-REQ-03-17-001
     * @rationale Bounded retry prevents infinite GEDR write attempts.
     */
    bool HandleGedrRetryOnError(int8_t returnCode);

    /**
     * @brief Acts on fatal GEDR API error codes by aborting GEDR transmission.
     * @details Handles unrecoverable GEDR errors by stopping further GEDR writes.
     * @param[in] returnCode Fatal GEDR API return code. Valid range: [INT8_MIN, INT8_MAX].
     * @throws None
     * @pre GEDR write returned a fatal error code.
     * @post GEDR transmission for this event is aborted.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-17-001
     * @rationale Fatal errors indicate unrecoverable GEDR storage issues.
     */
    void HandleGedrFatalError(int8_t returnCode);

    /**
     * @brief Creates header-only dummy data with length 0 when camera image collection fails.
     * @details Builds a minimal image data payload with zero-length body for GEDR.
     * @return Dummy image data vector containing only the header.
     * @throws None
     * @pre Image acquisition has failed or timed out.
     * @post Returned vector contains a valid header with zero-length image data.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-17-006
     * @rationale GEDR requires an image data block even when no images are available.
     */
    std::vector<uint8_t> BuildDummyImageDataForGedr() const;

    /**
     * @brief Clamps GEDR log start time to past data retention limit.
     * @details If startTime exceeds the retention limit, clamps to the limit value.
     * @param[in] startTime GEDR log start time in seconds. Valid range: [INT32_MIN, 0].
     * @return Clamped GEDR log start time.
     * @throws None
     * @pre None.
     * @post No state change.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-17-007
     * @rationale GEDR has its own retention limit that may differ from DAQ.
     */
    int32_t ValidateGedrLogStartTime(int32_t startTime) const noexcept;

    /**
     * @brief Clamps GEDR log data end time to -1 when greater than -1.
     * @details If endTime is greater than -1, returns -1. Otherwise returns endTime unchanged.
     * @param[in] endTime GEDR log end time in seconds. Valid range: [INT32_MIN, INT32_MAX].
     * @return Clamped GEDR log end time.
     * @retval -1 if endTime was greater than -1.
     * @retval Original endTime if already <= -1.
     * @throws None
     * @pre None.
     * @post No state change.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-17-008
     * @rationale GEDR log end time must not exceed -1 per protocol specification.
     */
    int32_t ValidateGedrLogEndTime(int32_t endTime) const noexcept;

    /**
     * @brief Does not send log data to GEDR API when log data start time is greater than -1.
     * @details Checks if the start time indicates invalid data that should be suppressed.
     * @param[in] startTime Log data start time in seconds. Valid range: [INT32_MIN, INT32_MAX].
     * @return True if data should be suppressed (start time > -1), false otherwise.
     * @retval true  Start time is invalid — suppress GEDR data.
     * @retval false Start time is valid — proceed with GEDR transmission.
     * @throws None
     * @pre None.
     * @post No state change.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-17-009
     * @rationale Invalid start time indicates no valid log data is available for GEDR.
     */
    bool SuppressGedrDataWhenStartTimeInvalid(int32_t startTime) const noexcept;

    /* ===================================================================
     * Private Methods — Dual Trigger Ordering
     * =================================================================== */

    /**
     * @brief Ensures correct DAQ-before-GEDR ordering for both probe and GEDR triggers.
     * @details Manages the dual-target transmission state machine.
     * @throws None
     * @pre Event data payloads have been constructed for both DAQ and GEDR.
     * @post Transmission proceeds in DAQ-first, GEDR-second order.
     * @note Called by: ProbeComm internally.
     * @requirements SWR-REQ-03-18-001;SWR-REQ-03-18-002;SWR-REQ-03-18-003;SWR-REQ-03-18-004;SWR-REQ-03-18-005
     * @rationale DAQ must receive data before GEDR to maintain system consistency.
     */
    void EnforceDualTriggerTransmissionOrder();

    /* ===================================================================
     * Private Methods — ZAT Status Reporting
     * =================================================================== */

    /**
     * @brief Indicates trigger rejection when maximum duplicates exceeded.
     * @details Sends the ADAS_ACore_PROBE2ZAT_EVT message with rejection indication.
     * @throws None
     * @pre probeEventStt service has been offered.
     * @post Rejection status is sent to ZAT.
     * @note Called by: ProbeComm internally.
     * @see SendTriggerAcceptanceStatusToZAT
     * @requirements SWR-REQ-03-05-002
     * @rationale ZAT must be informed when triggers are rejected for arbitration.
     */
    void SendTriggerRejectionToZAT();

    /* ===================================================================
     * Private Methods — Data Lifecycle
     * =================================================================== */

    /**
     * @brief Removes development event data from memory after successful DAQ transmission confirmation.
     * @details Clears event data buffers after DAQ confirms successful receipt.
     * @throws None
     * @pre DAQ has confirmed successful receipt of all event data.
     * @post Event data buffers are cleared.
     * @note Called by: ProbeComm internally.
     * @see CheckRetainDataUntilAllTargetsComplete
     * @requirements SWR-REQ-03-10-001
     * @rationale Memory must be freed after successful transmission to prevent exhaustion.
     */
    void DeleteEventDataAfterDaqSuccess();

    /**
     * @brief Removes development event data from memory after successful GEDR transmission.
     * @details Clears event data buffers after GEDR confirms successful write.
     * @throws None
     * @pre GEDR has confirmed successful write of all event data.
     * @post Event data buffers are cleared.
     * @note Called by: ProbeComm internally.
     * @see CheckRetainDataUntilAllTargetsComplete
     * @requirements SWR-REQ-03-10-002
     * @rationale Memory must be freed after successful transmission to prevent exhaustion.
     */
    void DeleteEventDataAfterGedrSuccess();

    /**
     * @brief Discards event data when maximum retries are exhausted.
     * @details Clears event data buffers when retry limits are reached.
     * @throws None
     * @pre Maximum retry count has been exhausted for DAQ or GEDR.
     * @post Event data buffers are cleared.
     * @note Called by: ProbeComm internally.
     * @see AbortEventDataTransmissionOnRetryExhaustion
     * @requirements SWR-REQ-03-09-004;SWR-REQ-03-11-004
     * @rationale Data must be discarded when transmission is no longer possible.
     */
    void DeleteEventDataOnRetryExhaustion();

    /**
     * @brief Retains event data until transmission to all designated destinations is confirmed successful.
     * @details Checks if both DAQ and GEDR transmissions are complete before allowing deletion.
     * @return True if data should be retained (not all targets complete), false if safe to delete.
     * @retval true  Data must be retained — not all targets have confirmed receipt.
     * @retval false All targets confirmed — data can be safely deleted.
     * @throws None
     * @pre At least one transmission target has completed.
     * @post No state change.
     * @note Called by: ProbeComm internally.
     * @see DeleteEventDataAfterDaqSuccess, DeleteEventDataAfterGedrSuccess
     * @requirements SWR-REQ-03-10-003
     * @rationale Data must persist until all destinations confirm receipt.
     */
    bool CheckRetainDataUntilAllTargetsComplete() const noexcept;

    /* ===================================================================
     * Private Methods — Development Logging
     * =================================================================== */

    /**
     * @brief Writes data sent to DAQ into log file with _snd_YYYYMMDD-HHMMSS.log naming.
     * @details Appends the sent data to the development log file if devLogEnabled_ is true.
     * @param[in] data Data bytes that were sent to DAQ. Must not be empty.
     * @throws None
     * @pre devLogEnabled_ is true.
     * @post Data is appended to the send log file.
     * @note Called by: ProbeComm internally.
     * @warning Only active when devLogEnabled_ is true.
     * @see EnableDevLogFileOutput
     * @requirements SWR-REQ-01-11-001;SWR-REQ-01-11-002;SWR-REQ-03-15-001
     * @rationale Development logging aids debugging of DAQ communication.
     */
    void WriteDevLogSentData(const std::vector<uint8_t>& data);

    /**
     * @brief Writes data received from DAQ into log file with _rcv_YYYYMMDD-HHMMSS.log naming.
     * @details Appends the received data to the development log file if devLogEnabled_ is true.
     * @param[in] data Data bytes that were received from DAQ. Must not be empty.
     * @throws None
     * @pre devLogEnabled_ is true.
     * @post Data is appended to the receive log file.
     * @note Called by: ProbeComm internally.
     * @warning Only active when devLogEnabled_ is true.
     * @see EnableDevLogFileOutput
     * @requirements SWR-REQ-03-15-002
     * @rationale Development logging aids debugging of DAQ communication.
     */
    void WriteDevLogReceivedData(const std::vector<uint8_t>& data);

    /* ===================================================================
     * Private Member Variables
     * =================================================================== */

    /// @brief Flag indicating DAQ SOME/IP communication is established. Range: {true, false}.
    bool daqCommunicationEstablished_{false};

    /// @brief Flag indicating a ZAT trigger has been received. Range: {true, false}.
    bool zatTriggerReceived_{false};

    /// @brief Flag indicating CameraHost answer back has been received. Range: {true, false}.
    bool cameraAnswerBackReceived_{false};

    /// @brief Flag indicating all 14 camera images have been received or timeout occurred. Range: {true, false}.
    bool imageAcquisitionComplete_{false};

    /// @brief BDP upload request retry counter. Range: [0, bdpMaxRetryCount_].
    uint32_t bdpRequestRetryCounter_{0U};

    /// @brief BDP data transmit retry counter. Range: [0, bdpMaxRetryCount_].
    uint32_t bdpTransmitRetryCounter_{0U};

    /// @brief GEDR write retry counter. Range: [0, gedrMaxRetryCount_].
    uint32_t gedrWriteRetryCounter_{0U};

    /// @brief Number of currently active event processes. Range: [0, allowedOverlapTriggerNum_].
    uint32_t activeEventProcessCount_{0U};

    /// @brief Map of category ID to trigger acceptance count per driving cycle.
    std::map<uint32_t, uint32_t> categoryTriggerCountMap_null;

    /// @brief Current data transmission sequence number. Range: [0, UINT32_MAX].
    uint32_t currentDataTransmissionNum_{0U};

    /// @brief Current category identifier for the active event. Range: [0, UINT32_MAX].
    uint32_t currentCategoryIdentifier_{0U};

    /// @brief Data upload request flag from ZAT trigger. Range: [0, 255].
    uint8_t dataUploadRequestFlg_{0U};

    /// @brief Effective log data start time in seconds (clamped). Range: [INT32_MIN, 0].
    int32_t effectiveLogStartTime_{0};

    /// @brief Effective log data end time in seconds (clamped). Range: [0, 83].
    int32_t effectiveLogEndTime_{0};

    /// @brief Current image frame counter within the 14-image sequence. Range: [0, 14].
    uint16_t imageFrameCounter_{0U};

    /// @brief Number of camera images received so far. Range: [0, 14].
    uint16_t receivedImageCount_{0U};

    /// @brief Event data frame counter for payload construction. Range: [0, UINT32_MAX].
    uint32_t eventDataFrameCounter_{0U};

    /// @brief Current segment number within a segmented payload. Range: [0, totalSegments_].
    uint32_t segmentNumber_{0U};

    /// @brief Total number of segments for the current payload. Range: [0, UINT32_MAX].
    uint32_t totalSegments_{0U};

    /// @brief Last accepted data transmission number reported to ZAT. Range: [0, UINT32_MAX].
    uint32_t acceptedDataTransmissionNum_{0U};

    /// @brief Last accepted category identifier reported to ZAT. Range: [0, UINT32_MAX].
    uint32_t acceptedCategoryIdentifier_{0U};

    /// @brief Allowed number of overlapping trigger events. Range: [0, UINT32_MAX].
    uint32_t allowedOverlapTriggerNum_{0U};

    /// @brief Configurable image acquisition timeout in seconds. Default: 55. Range: [0, UINT32_MAX]. Units: seconds.
    uint32_t imageAcquisitionTimeoutSec_{55U};

    /// @brief Configurable BDP retry interval in milliseconds. Range: [0, UINT32_MAX]. Units: milliseconds.
    uint32_t bdpRetryIntervalMs_{0U};

    /// @brief Maximum number of BDP request/transmit retries. Range: [0, UINT32_MAX].
    uint32_t bdpMaxRetryCount_{0U};

    /// @brief Maximum number of GEDR write retries. Range: [0, UINT32_MAX].
    uint32_t gedrMaxRetryCount_{0U};

    /// @brief Configurable per-category trigger acceptance limit per driving cycle. Range: [0, UINT32_MAX].
    uint32_t sameCategoryLimitPerDC_{0U};

    /// @brief Flag indicating development log file output is enabled. Range: {true, false}.
    bool devLogEnabled_{false};

    /// @brief Flag indicating image acquisition timeout timer has been started. Range: {true, false}.
    bool imageTimeoutTimerStarted_{false};

    /// @brief Mutex protecting shared send buffer access for thread safety.
    mutable std::mutex sendBufferMutex_null;

    /// @brief Mutex protecting camera image reception state for thread safety.
    mutable std::mutex cameraMutex_null;
};

} // namespace probe