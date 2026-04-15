#pragma once

/**
 * @file    ProbeComm.hpp
 * @brief   Communication layer for the ProbeComm component handling DAQ, ZAT, CameraHost, GEDR, and SDMAP services.
 * @details ProbeComm manages all SOME/IP service discovery, event handling, data payload construction,
 *          segmentation, and transmission to DAQ and GEDR targets. It orchestrates steady-state continual
 *          data, BDP event data upload, camera image acquisition, and trigger acceptance status reporting
 *          to ZAT. Thread-safe access to shared buffers is enforced via std::mutex members.
 * @author  Engineering Team
 * @date    2024-01-01
 * @version 1.0.0
 * @copyright Copyright (c) 2024 Company. All rights reserved.
 */

#include <cstdint>
#include <vector>
#include <map>
#include <mutex>
#include <optional>
#include <atomic>
#include <string>
#include <chrono>

/**
 * @defgroup ProbeCommModule ProbeComm Communication Module
 * @brief Components responsible for probe communication, service discovery, data transmission, and event handling.
 */

/// @namespace probe
/// @brief Contains all components of the probe application.
namespace probe {

/**
 * @class   ProbeCommVariant
 * @brief   Variant configuration holder for ProbeComm initialization.
 * @details Carries variant-specific configuration parameters required during
 *          ProbeComm construction and communication setup.
 * @ingroup ProbeCommModule
 * @note    Lifetime must exceed that of the ProbeComm instance that references it.
 */
class ProbeCommVariant;

/**
 * @class   ProbeComm
 * @brief   Communication manager for the Probe application handling all service interfaces.
 * @details ProbeComm is responsible for discovering and subscribing to SOME/IP services
 *          (DAQ, ZAT, CameraHost, TSR, HDF, SDMAP), offering probe services to ZAT and
 *          CameraHost, constructing and segmenting event data payloads, managing BDP upload
 *          request/transmit retry logic, orchestrating GEDR writes, enforcing image acquisition
 *          timeouts, and reporting trigger acceptance status. All shared state is protected by
 *          dedicated std::mutex members. The component is designed for single-threaded periodic
 *          invocation from a 10 ms task, with internal state machines handling multi-cycle operations.
 * @ingroup ProbeCommModule
 * @note    Complies with ISO 26262 ASIL-B, MISRA C++ guidelines, and C++14/17 standards.
 *          No dynamic memory allocation via raw new/delete; std::vector managed on heap via RAII.
 * @warning Do not call public methods from multiple threads without external synchronization
 *          beyond the internal mutexes provided.
 * @invariant daqCommunicationEstablished_ reflects the current SOME/IP DAQ link state.
 *            bdpRequestRetryCounter_ <= bdpMaxRetryCount_ at all times.
 *            bdpTransmitRetryCounter_ <= bdpMaxRetryCount_ at all times.
 *            gedrWriteRetryCounter_ <= gedrMaxRetryCount_ at all times.
 *            activeEventProcessCount_ >= 0 at all times.
 * @see     ProbeCommVariant
 */
class ProbeComm {
public:

    // =========================================================================
    // Construction
    // =========================================================================

    /**
     * @brief   Constructs ProbeComm with a variant reference for communication setup.
     * @details Initializes all internal state variables to safe defaults, stores the
     *          variant pointer for configuration access, and prepares the communication
     *          layer for subsequent service discovery calls. No SOME/IP operations are
     *          performed during construction.
     * @param[in] variant  Non-owning pointer to the ProbeCommVariant configuration object.
     *                     Must not be nullptr. Lifetime must exceed this object's lifetime.
     * @pre     variant != nullptr.
     * @post    All internal counters are zero. daqCommunicationEstablished_ is false.
     *          devLogEnabled_ is false.
     * @throws  None — constructor is noexcept.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @warning Passing a nullptr variant results in undefined behavior.
     * @requirements SWR-REQ-01-04-001;SWR-REQ-03-16-002
     * @rationale Variant pointer injection allows compile-time variant selection without
     *            dynamic dispatch overhead.
     * @see     FindServiceDaqContinual
     */
    explicit ProbeComm(ProbeCommVariant* variant) noexcept;

    /**
     * @brief   Destroys the ProbeComm instance and releases all managed resources.
     * @details Ensures all mutexes are released and any pending state is cleaned up.
     * @throws  None.
     */
    ~ProbeComm() noexcept;

    /// @brief Deleted copy constructor — ProbeComm is non-copyable.
    ProbeComm(const ProbeComm&) = delete;

    /// @brief Deleted copy assignment — ProbeComm is non-copyable.
    ProbeComm& operator=(const ProbeComm&) = delete;

    /// @brief Deleted move constructor — ProbeComm is non-movable.
    ProbeComm(ProbeComm&&) = delete;

    /// @brief Deleted move assignment — ProbeComm is non-movable.
    ProbeComm& operator=(ProbeComm&&) = delete;

    // =========================================================================
    // Service Discovery — Provided: CORE_DAQ_continual_Service
    // =========================================================================

    /**
     * @brief   Locates the DAQ SOME/IP continual data service for steady-state data transmission.
     * @details Initiates asynchronous service discovery for the DAQ continual data SOME/IP service.
     *          Upon successful discovery, sets the internal communication-established flag and
     *          enables steady-state data transmission. Should be called once at startup.
     * @pre     ProbeComm has been constructed with a valid variant.
     * @post    Internal service handle for DAQ continual is populated on success.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @warning Data suppression (SuppressDataBeforeDaqEstablished) remains active until this
     *          call completes successfully.
     * @requirements SWR-REQ-01-04-001;SWR-REQ-01-06-003;SWR-REQ-03-16-002
     * @rationale Separate discovery per service allows independent retry and fault isolation.
     * @see     SuppressDataBeforeDaqEstablished, SendContinualShortDataCyclic100ms
     */
    void FindServiceDaqContinual() noexcept;

    /**
     * @brief   Locates the DAQ SOME/IP development event data service for BDP event transmission.
     * @details Initiates asynchronous service discovery for the DAQ development event SOME/IP
     *          service. Upon successful discovery, enables BDP upload request and transmit
     *          operations. Should be called once at startup.
     * @pre     ProbeComm has been constructed with a valid variant.
     * @post    Internal service handle for DAQ devEvent is populated on success.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @requirements SWR-REQ-01-04-001;SWR-REQ-03-16-002
     * @rationale Separate discovery per service allows independent retry and fault isolation.
     * @see     SendBdpUploadRequestToDAQ, TransmitBdpDataToDAQ
     */
    void FindServiceDaqDevEvent() noexcept;

    /**
     * @brief   Portable entry point used by ProbeApp to start DAQ continual SOME/IP discovery.
     * @return  true when DAQ communication is established after this call (stub may set sync).
     */
    bool InitiateSomeIpDiscovery() noexcept;

    // =========================================================================
    // Service Discovery — Provided: ZAT_hdfProbeBusOut_Service
    // =========================================================================

    /**
     * @brief   Locates the ZAT trigger service for receiving event triggers at 100 ms periodic rate.
     * @details Initiates service discovery for the ZAT hdfProbeBusOut trigger SOME/IP service.
     *          Registers HandleAdasAcoreTriggerEvt as the event callback upon successful discovery.
     * @pre     ProbeComm has been constructed with a valid variant.
     * @post    ZAT trigger event subscription is active on success.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @requirements SWR-REQ-03-02-001;SWR-REQ-03-003
     * @rationale ZAT trigger drives the core event detection pipeline.
     * @see     HandleAdasAcoreTriggerEvt
     */
    void FindServiceZatTrigger() noexcept;

    /**
     * @brief   Locates ZAT regular upload data service for receiving regular upload data.
     * @details Initiates service discovery for the ZAT hdfProbeBusOut regular upload SOME/IP
     *          service. Registers HandleZatRegularUploadData1Evt as the event callback upon
     *          successful discovery.
     * @pre     ProbeComm has been constructed with a valid variant.
     * @post    ZAT regular upload event subscription is active on success.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @requirements SWR-REQ-01-01-001
     * @rationale Regular upload data feeds the steady-state signal collection pipeline.
     * @see     HandleZatRegularUploadData1Evt
     */
    void FindServiceZatRegularUpload() noexcept;

    // =========================================================================
    // Service Discovery — Provided: CameraHost_IMG_Service
    // =========================================================================

    /**
     * @brief   Locates the CameraHost image service for receiving camera images.
     * @details Initiates service discovery for the CameraHost IMG SOME/IP service.
     *          Registers HandleCam2ProbePictureEvt and HandleCam2ProbeAnswerBackEvt as
     *          event callbacks upon successful discovery.
     * @pre     ProbeComm has been constructed with a valid variant.
     * @post    CameraHost image event subscriptions are active on success.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @requirements SWR-REQ-03-06-002;SWR-REQ-03-06-006
     * @see     HandleCam2ProbePictureEvt, HandleCam2ProbeAnswerBackEvt
     */
    void FindServiceCameraImg() noexcept;

    // =========================================================================
    // Service Discovery — Provided: CameraHost_AGhdf_Service
    // =========================================================================

    /**
     * @brief   Locates the CameraHost A-HDF/G-HDF data service for receiving camera object data.
     * @details Initiates service discovery for the CameraHost AGhdf SOME/IP service.
     *          Registers FC camera bus-out event callbacks upon successful discovery.
     * @pre     ProbeComm has been constructed with a valid variant.
     * @post    CameraHost AGhdf event subscriptions are active on success.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @requirements SWR-REQ-03-01-002
     * @see     HandleCam2ProbeFcCamBusOutEvt
     */
    void FindServiceCameraAghdf() noexcept;

    // =========================================================================
    // Service Discovery — Provided: TSR2GCoreHDF_Service
    // =========================================================================

    /**
     * @brief   Locates the TSR2GCoreHDF service for receiving TSR data.
     * @details Initiates service discovery for the TSR2GCoreHDF SOME/IP service.
     *          Registers HandleTsr2GCoreHdfEvt as the event callback upon successful discovery.
     * @pre     ProbeComm has been constructed with a valid variant.
     * @post    TSR2GCoreHDF event subscription is active on success.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @requirements SWR-REQ-03-01-002
     * @see     HandleTsr2GCoreHdfEvt
     */
    void FindServiceTsr2GCoreHdf() noexcept;

    // =========================================================================
    // Service Discovery — Provided: TSR2HDF_Service
    // =========================================================================

    /**
     * @brief   Locates the TSR2HDF service for receiving TSR ACore data.
     * @details Initiates service discovery for the TSR2HDF SOME/IP service.
     *          Registers HandleTsr2HdfEvt as the event callback upon successful discovery.
     * @pre     ProbeComm has been constructed with a valid variant.
     * @post    TSR2HDF event subscription is active on success.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @requirements SWR-REQ-03-01-002
     * @see     HandleTsr2HdfEvt
     */
    void FindServiceTsr2Hdf() noexcept;

    // =========================================================================
    // Service Discovery — Provided: hdfAp2CpBusOut_Service
    // =========================================================================

    /**
     * @brief   Locates the hdfAp2CpBusOut service for receiving HDF AP to CP data.
     * @details Initiates service discovery for the hdfAp2CpBusOut SOME/IP service.
     *          Registers HandleHdfAp2CpBusOutEvt as the event callback upon successful discovery.
     * @pre     ProbeComm has been constructed with a valid variant.
     * @post    hdfAp2CpBusOut event subscription is active on success.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @requirements SWR-REQ-03-01-002
     * @see     HandleHdfAp2CpBusOutEvt
     */
    void FindServiceHdfAp2CpBusOut() noexcept;

    // =========================================================================
    // Service Discovery — Provided: hdfCp2ApBusOut_Service
    // =========================================================================

    /**
     * @brief   Locates the hdfCp2ApBusOut service for receiving ICCOMMW CP to AP data.
     * @details Initiates service discovery for the hdfCp2ApBusOut SOME/IP service.
     *          Registers HandleHdfCp2ApBusOutEvt as the event callback upon successful discovery.
     * @pre     ProbeComm has been constructed with a valid variant.
     * @post    hdfCp2ApBusOut event subscription is active on success.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @requirements SWR-REQ-03-01-002
     * @see     HandleHdfCp2ApBusOutEvt
     */
    void FindServiceHdfCp2ApBusOut() noexcept;

    // =========================================================================
    // Service Discovery — Provided: SDMAP_Service
    // =========================================================================

    /**
     * @brief   Locates the SDMAP data service for receiving map data.
     * @details Initiates service discovery for the SDMAP SOME/IP service.
     *          Registers HandleSdmapDataEvt as the event callback upon successful discovery.
     * @pre     ProbeComm has been constructed with a valid variant.
     * @post    SDMAP event subscription is active on success.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @requirements SWR-REQ-03-01-002
     * @see     HandleSdmapDataEvt
     */
    void FindServiceSdmap() noexcept;

    // =========================================================================
    // Service Offering — Provided: probeEventStt_Service
    // =========================================================================

    /**
     * @brief   Offers the probeEventStt service to allow ZAT to subscribe for trigger acceptance status.
     * @details Registers and offers the probeEventStt SOME/IP service so that ZAT can subscribe
     *          and receive periodic trigger acceptance status events (ADAS_ACore_PROBE2ZAT_EVT).
     * @pre     ProbeComm has been constructed with a valid variant.
     * @post    probeEventStt service is offered and available for ZAT subscription.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @requirements SWR-REQ-03-05-001;SWR-REQ-03-05-003
     * @see     SendTriggerAcceptanceStatusToZAT, SendAllowedOverlapCountToZAT
     */
    void OfferServiceProbeEventStt() noexcept;

    // =========================================================================
    // Service Offering — Provided: PROBE_TRG_Service
    // =========================================================================

    /**
     * @brief   Offers the PROBE_TRG_Service to allow CameraHost to subscribe for image acquisition triggers.
     * @details Registers and offers the PROBE_TRG SOME/IP service so that CameraHost can subscribe
     *          and receive image acquisition trigger events (ADAS_PROBE2CAM_EVT).
     * @pre     ProbeComm has been constructed with a valid variant.
     * @post    PROBE_TRG service is offered and available for CameraHost subscription.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @requirements SWR-REQ-03-06-001
     * @see     SendImageAcquisitionTriggerToCameraHost
     */
    void OfferServiceProbeTrigger() noexcept;

    // =========================================================================
    // Event Handlers — ZAT_hdfProbeBusOut_Service
    // =========================================================================

    /**
     * @brief   Receives and processes development event triggers from ZAT at 100 ms periodic cycle.
     * @details Deserializes the trigger data vector, validates the upload request flag, checks
     *          category and overlap limits, clamps log start/end times, and initiates the event
     *          data generation pipeline. Sets zatTriggerReceived_ and updates internal trigger
     *          state fields.
     * @param[in] triggerData  Serialized ZAT trigger payload. Must not be empty.
     *                         Valid range: non-empty byte vector conforming to ZAT trigger format.
     * @pre     FindServiceZatTrigger has been called and subscription is active.
     * @post    Internal trigger state is updated. Event pipeline may be initiated.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Cyclic_100ms.
     * @warning Must not block; all heavy processing is deferred to cyclic tasks.
     * @requirements SWR-REQ-03-02-001;SWR-REQ-03-02-002;SWR-REQ-03-003
     * @see     ValidateTriggerUploadRequestFlag, CheckSameCategoryLimit, CheckOverlapTriggerLimit
     */
    void HandleAdasAcoreTriggerEvt(std::vector<uint8_t> triggerData) noexcept;

    /**
     * @brief   Receives ZAT regular upload data for signal collection.
     * @details Deserializes the regular upload data vector and stores it for inclusion in
     *          the steady-state continual data payload.
     * @param[in] data  Serialized ZAT regular upload payload. Must not be empty.
     *                  Valid range: non-empty byte vector conforming to ZAT regular upload format.
     * @pre     FindServiceZatRegularUpload has been called and subscription is active.
     * @post    Internal regular upload data buffer is updated.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Cyclic_100ms.
     * @requirements SWR-REQ-01-01-001
     * @see     SendContinualShortDataCyclic100ms
     */
    void HandleZatRegularUploadData1Evt(std::vector<uint8_t> data) noexcept;

    // =========================================================================
    // Event Handlers — CameraHost_IMG_Service
    // =========================================================================

    /**
     * @brief   Receives and evaluates the answer back from CameraHost after image acquisition trigger.
     * @details Deserializes the answer back message, calls ValidateCameraAnswerBack to evaluate
     *          the acceptance status, and updates cameraAnswerBackReceived_. On rejection, calls
     *          HandleCameraErrorCode and AbortImageProcessingBeforeTransmitPhase.
     * @param[in] answerData  Serialized CameraHost answer back payload. Must not be empty.
     *                        Valid range: non-empty byte vector conforming to CameraHost answer back format.
     * @pre     FindServiceCameraImg has been called. SendImageAcquisitionTriggerToCameraHost
     *          has been called for the current event.
     * @post    cameraAnswerBackReceived_ is set. Image acquisition proceeds or is aborted.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-06-002;SWR-REQ-03-06-003;SWR-REQ-03-06-004
     * @see     ValidateCameraAnswerBack, HandleCameraErrorCode, AbortImageProcessingBeforeTransmitPhase
     */
    void HandleCam2ProbeAnswerBackEvt(std::vector<uint8_t> answerData) noexcept;

    /**
     * @brief   Receives camera images from CameraHost with timestamp and frame counter.
     * @details Deserializes the image data, validates the frame counter and timestamp,
     *          stores the image in the internal image buffer, and increments receivedImageCount_.
     *          Checks EnforceImageAcquisitionTimeout on each reception.
     * @param[in] imageData  Serialized camera image payload including timestamp and frame counter.
     *                       Must not be empty. Valid range: non-empty byte vector conforming to
     *                       CameraHost image format.
     * @pre     FindServiceCameraImg has been called. cameraAnswerBackReceived_ is true.
     * @post    receivedImageCount_ is incremented. imageAcquisitionComplete_ may be set to true
     *          when all 14 images are received.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-06-006;SWR-REQ-03-06-007;SWR-REQ-03-06-008;SWR-REQ-03-06-009
     * @see     ReceiveCameraImages, EnforceImageAcquisitionTimeout, CopyImageDataAligned4Bytes
     */
    void HandleCam2ProbePictureEvt(std::vector<uint8_t> imageData) noexcept;

    /**
     * @brief   Receives camera FC object data from CameraHost.
     * @details Deserializes the FC camera bus-out data and stores it for event signal collection.
     * @param[in] data  Serialized FC camera bus-out payload. Must not be empty.
     *                  Valid range: non-empty byte vector conforming to FC camera bus-out format.
     * @pre     FindServiceCameraAghdf has been called and subscription is active.
     * @post    Internal FC camera data buffer is updated.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-01-002
     */
    void HandleCam2ProbeFcCamBusOutEvt(std::vector<uint8_t> data) noexcept;

    /**
     * @brief   Receives fused camera object data from CameraHost.
     * @details Deserializes the fused camera CMBS FCV bus-out data and stores it for event
     *          signal collection.
     * @param[in] data  Serialized fused camera CMBS FCV bus-out payload. Must not be empty.
     *                  Valid range: non-empty byte vector conforming to fused camera format.
     * @pre     FindServiceCameraAghdf has been called and subscription is active.
     * @post    Internal fused camera FCV data buffer is updated.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-01-002
     */
    void HandleCam2ProbeFcCamCmbsFcvBusOutEvt(std::vector<uint8_t> data) noexcept;

    /**
     * @brief   Receives lane camera data from CameraHost.
     * @details Deserializes the lane camera CMBS LN bus-out data and stores it for event
     *          signal collection.
     * @param[in] data  Serialized lane camera CMBS LN bus-out payload. Must not be empty.
     *                  Valid range: non-empty byte vector conforming to lane camera format.
     * @pre     FindServiceCameraAghdf has been called and subscription is active.
     * @post    Internal lane camera data buffer is updated.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-01-002
     */
    void HandleCam2ProbeFcCamCmbsLnBusOutEvt(std::vector<uint8_t> data) noexcept;

    /**
     * @brief   Receives object camera data from CameraHost.
     * @details Deserializes the object camera CMBS OBJ bus-out data and stores it for event
     *          signal collection.
     * @param[in] data  Serialized object camera CMBS OBJ bus-out payload. Must not be empty.
     *                  Valid range: non-empty byte vector conforming to object camera format.
     * @pre     FindServiceCameraAghdf has been called and subscription is active.
     * @post    Internal object camera data buffer is updated.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-01-002
     */
    void HandleCam2ProbeFcCamCmbsObjBusOutEvt(std::vector<uint8_t> data) noexcept;

    // =========================================================================
    // Event Handlers — TSR2GCoreHDF_Service
    // =========================================================================

    /**
     * @brief   Receives TSR GCore data for event signal collection.
     * @details Deserializes the TSR GCore HDF data and stores it in the internal buffer
     *          for inclusion in the event log data set.
     * @param[in] data  Serialized TSR GCore HDF payload. Must not be empty.
     *                  Valid range: non-empty byte vector conforming to TSR GCore HDF format.
     * @pre     FindServiceTsr2GCoreHdf has been called and subscription is active.
     * @post    Internal TSR GCore data buffer is updated.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-01-002
     */
    void HandleTsr2GCoreHdfEvt(std::vector<uint8_t> data) noexcept;

    // =========================================================================
    // Event Handlers — TSR2HDF_Service
    // =========================================================================

    /**
     * @brief   Receives TSR ACore data for event signal collection.
     * @details Deserializes the TSR HDF data and stores it in the internal buffer for
     *          inclusion in the event log data set.
     * @param[in] data  Serialized TSR HDF payload. Must not be empty.
     *                  Valid range: non-empty byte vector conforming to TSR HDF format.
     * @pre     FindServiceTsr2Hdf has been called and subscription is active.
     * @post    Internal TSR ACore data buffer is updated.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-01-002
     */
    void HandleTsr2HdfEvt(std::vector<uint8_t> data) noexcept;

    // =========================================================================
    // Event Handlers — hdfAp2CpBusOut_Service
    // =========================================================================

    /**
     * @brief   Receives HDF AP to CP bus output data.
     * @details Deserializes the HDF AP to CP bus-out data and stores it in the internal
     *          buffer for inclusion in the event log data set.
     * @param[in] data  Serialized HDF AP to CP bus-out payload. Must not be empty.
     *                  Valid range: non-empty byte vector conforming to HDF AP to CP format.
     * @pre     FindServiceHdfAp2CpBusOut has been called and subscription is active.
     * @post    Internal HDF AP to CP data buffer is updated.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-01-002
     */
    void HandleHdfAp2CpBusOutEvt(std::vector<uint8_t> data) noexcept;

    // =========================================================================
    // Event Handlers — hdfCp2ApBusOut_Service
    // =========================================================================

    /**
     * @brief   Receives ICCOMMW CP to AP communication data.
     * @details Deserializes the HDF CP to AP bus-out data and stores it in the internal
     *          buffer for inclusion in the event log data set.
     * @param[in] data  Serialized HDF CP to AP bus-out payload. Must not be empty.
     *                  Valid range: non-empty byte vector conforming to HDF CP to AP format.
     * @pre     FindServiceHdfCp2ApBusOut has been called and subscription is active.
     * @post    Internal HDF CP to AP data buffer is updated.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-01-002
     */
    void HandleHdfCp2ApBusOutEvt(std::vector<uint8_t> data) noexcept;

    // =========================================================================
    // Event Handlers — SDMAP_Service
    // =========================================================================

    /**
     * @brief   Receives SDMAP map data for event signal collection.
     * @details Deserializes the SDMAP data and stores it in the internal buffer for
     *          inclusion in the event log data set.
     * @param[in] data  Serialized SDMAP payload. Must not be empty.
     *                  Valid range: non-empty byte vector conforming to SDMAP data format.
     * @pre     FindServiceSdmap has been called and subscription is active.
     * @post    Internal SDMAP data buffer is updated.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-01-002
     */
    void HandleSdmapDataEvt(std::vector<uint8_t> data) noexcept;

    // =========================================================================
    // Event Handlers — CORE_DAQ_devEvent_Service
    // =========================================================================

    /**
     * @brief   Receives and processes the BDP upload completion result code from DAQ.
     * @details Evaluates the result code to determine whether the BDP upload cycle completed
     *          successfully. On success, triggers DeleteEventDataAfterDaqSuccess. On failure,
     *          initiates retry or abort logic.
     * @param[in] resultCode  BDP upload result code received from DAQ.
     *                        Valid range: uint16_t; specific values defined by DAQ protocol.
     * @pre     TransmitBdpDataToDAQ has been called for the current event.
     * @post    Event data is deleted on success, or retry/abort is initiated on failure.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-10-001
     * @see     DeleteEventDataAfterDaqSuccess, EnforceBdpTransmitRetry
     */
    void HandleResultBdpEvt(uint16_t resultCode) noexcept;

    // =========================================================================
    // Continual Data Transmission — CORE_DAQ_continual_Service
    // =========================================================================

    /**
     * @brief   Transmits short-cycle steady-state data to DAQ via SOME/IP UDP at 100 ms intervals.
     * @details Checks SuppressDataBeforeDaqEstablished before transmitting. If DAQ communication
     *          is established, serializes the payload and calls SendDaqFireForgetMethod.
     *          Optionally writes to dev log if devLogEnabled_ is true.
     * @param[in] payload  Serialized short-cycle steady-state data payload. Must not be empty.
     *                     Valid range: non-empty byte vector conforming to DAQ continual short format.
     * @pre     FindServiceDaqContinual has been called. daqCommunicationEstablished_ is true.
     * @post    Payload is transmitted to DAQ via SOME/IP UDP fire-and-forget.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Cyclic_100ms.
     * @warning Data is silently suppressed if DAQ communication is not yet established.
     * @requirements SWR-REQ-01-001;SWR-REQ-01-06-001;SWR-REQ-01-05-001
     * @see     SuppressDataBeforeDaqEstablished, SendDaqFireForgetMethod
     */
    void SendContinualShortDataCyclic100ms(std::vector<uint8_t> payload) noexcept;

    /**
     * @brief   Transmits long-cycle steady-state data to DAQ via SOME/IP UDP at 1000 ms intervals.
     * @details Checks SuppressDataBeforeDaqEstablished before transmitting. If DAQ communication
     *          is established, serializes the payload and calls SendDaqFireForgetMethod.
     *          Optionally writes to dev log if devLogEnabled_ is true.
     * @param[in] payload  Serialized long-cycle steady-state data payload. Must not be empty.
     *                     Valid range: non-empty byte vector conforming to DAQ continual long format.
     * @pre     FindServiceDaqContinual has been called. daqCommunicationEstablished_ is true.
     * @post    Payload is transmitted to DAQ via SOME/IP UDP fire-and-forget.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Cyclic_1000ms.
     * @warning Data is silently suppressed if DAQ communication is not yet established.
     * @requirements SWR-REQ-01-002;SWR-REQ-01-06-001;SWR-REQ-01-05-001
     * @see     SuppressDataBeforeDaqEstablished, SendDaqFireForgetMethod
     */
    void SendContinualLongDataCyclic1000ms(std::vector<uint8_t> payload) noexcept;

    // =========================================================================
    // Camera Trigger — PROBE_TRG_Service
    // =========================================================================

    /**
     * @brief   Sends trigger message to CameraHost when event data generation starts.
     * @details Constructs the ADAS_PROBE2CAM_EVT message with the provided event information
     *          and number of events, then transmits it via the PROBE_TRG SOME/IP service.
     * @param[in] eventInfo  Event information field for the trigger message.
     *                       Valid range: uint32_t; event-specific encoding.
     * @param[in] numEvt     Number of events associated with this trigger.
     *                       Valid range: [0, 255].
     * @pre     OfferServiceProbeTrigger has been called. CameraHost has subscribed.
     * @post    ADAS_PROBE2CAM_EVT is transmitted to CameraHost. imageTimeoutTimerStarted_ is set.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-06-001;SWR-REQ-03-18-001
     * @see     ReceiveCameraImages, EnforceImageAcquisitionTimeout
     */
    void SendImageAcquisitionTriggerToCameraHost(uint32_t eventInfo, uint8_t numEvt) noexcept;

    // =========================================================================
    // Event Data Transmission — CORE_DAQ_devEvent_Service
    // =========================================================================

    /**
     * @brief   Transmits event data payloads at 1000 ms pacing intervals.
     * @details Drives the event data transmission state machine at each 1000 ms cycle.
     *          Enforces EnforceEventDataTransmissionOrder and calls the appropriate
     *          Send*ToDAQ and Send*ToGEDR methods based on current transmission state.
     * @pre     A valid event has been accepted and event data is ready for transmission.
     * @post    One or more event data segments are transmitted per cycle.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Cyclic_1000ms.
     * @requirements SWR-REQ-03-07-002;SWR-REQ-03-09-005
     * @see     EnforceEventDataTransmissionOrder, SendAdasCategoryDataToDAQ, SendLogDataSetsToDAQ
     */
    void SendEventDataAtCyclic1000ms() noexcept;

    // =========================================================================
    // ZAT Status Reporting — probeEventStt_Service
    // =========================================================================

    /**
     * @brief   Periodically sends accepted data transmission numbers and category identifiers to ZAT.
     * @details Constructs and transmits the ADAS_ACore_PROBE2ZAT_EVT message containing
     *          acceptedDataTransmissionNum_ and acceptedCategoryIdentifier_ via the
     *          probeEventStt SOME/IP service.
     * @pre     OfferServiceProbeEventStt has been called. ZAT has subscribed.
     * @post    ADAS_ACore_PROBE2ZAT_EVT is transmitted to ZAT.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Cyclic_100ms.
     * @requirements SWR-REQ-03-05-001;SWR-REQ-03-05-004
     * @see     SendTriggerRejectionToZAT, SendAllowedOverlapCountToZAT
     */
    void SendTriggerAcceptanceStatusToZAT() noexcept;

    /**
     * @brief   Periodically reports the allowed overlap trigger number to trigger arbitration function.
     * @details Constructs and transmits the ADAS_ACore_PROBE2ZAT_EVT message containing
     *          allowedOverlapTriggerNum_ via the probeEventStt SOME/IP service.
     * @pre     OfferServiceProbeEventStt has been called. ZAT has subscribed.
     * @post    ADAS_ACore_PROBE2ZAT_EVT with overlap count is transmitted to ZAT.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Cyclic_100ms.
     * @requirements SWR-REQ-03-05-003;SWR-REQ-03-04-004
     * @see     SendTriggerAcceptanceStatusToZAT
     */
    void SendAllowedOverlapCountToZAT() noexcept;

    // =========================================================================
    // Drive Cycle Management
    // =========================================================================

    /**
     * @brief   Resets all per-category acceptance counters to zero when a new driving cycle begins.
     * @details Clears all entries in categoryTriggerCountMap_ to reset the per-category
     *          acceptance count for the new drive cycle.
     * @pre     ProbeComm has been constructed.
     * @post    categoryTriggerCountMap_ is cleared. All category counters are zero.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @requirements SWR-REQ-03-12-004
     * @see     CheckSameCategoryLimit
     */
    void ResetCategoryCounterOnNewDriveCycle() noexcept;

    // =========================================================================
    // Development Log Control
    // =========================================================================

    /**
     * @brief   Enables DAQ data logging in development builds only.
     * @details Sets devLogEnabled_ to true, enabling WriteDevLogSentData and
     *          WriteDevLogReceivedData to write log files.
     * @pre     ProbeComm has been constructed.
     * @post    devLogEnabled_ is true.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     *          Must only be called in development builds; must not be called in production.
     * @requirements SWR-REQ-01-11-001;SWR-REQ-03-15-001
     * @see     DisableDevLogFileOutput, WriteDevLogSentData
     */
    void EnableDevLogFileOutput() noexcept;

    /**
     * @brief   Disables DAQ data logging for production builds.
     * @details Sets devLogEnabled_ to false, preventing WriteDevLogSentData and
     *          WriteDevLogReceivedData from writing log files.
     * @pre     ProbeComm has been constructed.
     * @post    devLogEnabled_ is false.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @requirements SWR-REQ-01-11-003;SWR-REQ-03-15-003
     * @see     EnableDevLogFileOutput
     */
    void DisableDevLogFileOutput() noexcept;

private:

    // =========================================================================
    // Constants
    // =========================================================================

    /**
     * @brief   Maximum log data end time clamp value in seconds.
     * @details Defined as +83 seconds per SWR-REQ-03-02-004. Any received end time
     *          exceeding this value is clamped to this constant.
     */
    static constexpr int32_t kMaxLogEndTimeSec{83};

    /**
     * @brief   Maximum log data end time for GEDR clamping in seconds.
     * @details Defined as -1 per SWR-REQ-03-17-008. GEDR log end time is clamped to -1
     *          when the received value is greater than -1.
     */
    static constexpr int32_t kGedrMaxLogEndTimeSec{-1};

    /**
     * @brief   Total number of camera images expected per event acquisition.
     * @details 14 JPEG images covering -5 to +8 seconds around the event per SWR-REQ-03-06-006.
     */
    static constexpr uint8_t kExpectedImageCount{14U};

    /**
     * @brief   Default image acquisition timeout in seconds.
     * @details Configurable 55-second timeout per SWR-REQ-03-06-010. Stored as default;
     *          runtime value is held in imageAcquisitionTimeoutSec_.
     */
    static constexpr uint32_t kDefaultImageAcquisitionTimeoutSec{55U};

    // =========================================================================
    // Private Methods
    // =========================================================================

    /**
     * @brief   Transmits DAQ_UNIVERSAL_ETH_DATA_FIR_FGT message with client ID, PDU ID, and data vector.
     * @details Constructs the fire-and-forget UDP message and dispatches it via the DAQ continual
     *          SOME/IP service. Acquires sendBufferMutex_ before transmission.
     * @param[in] clientId    DAQ client identifier. Valid range: uint16_t full range.
     * @param[in] pduId       PDU identifier for the message. Valid range: uint32_t full range.
     * @param[in] dataVector  Serialized data payload. Must not be empty.
     * @pre     daqCommunicationEstablished_ is true.
     * @post    DAQ_UNIVERSAL_ETH_DATA_FIR_FGT message is transmitted.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Cyclic_100ms.
     * @requirements SWR-REQ-01-005;SWR-REQ-01-06-002;SWR-REQ-01-05-002
     * @see     SendContinualShortDataCyclic100ms, SendContinualLongDataCyclic1000ms
     */
    void SendDaqFireForgetMethod(uint16_t clientId, uint32_t pduId,
                                 std::vector<uint8_t> dataVector) noexcept;

    /**
     * @brief   Returns true to suppress data transmission when SOME/IP communication with DAQ is not yet established.
     * @details Checks daqCommunicationEstablished_ and returns true if communication has not
     *          yet been established, indicating that data should be suppressed.
     * @return  bool — true if data should be suppressed, false if DAQ communication is established.
     * @retval  true   DAQ communication is not yet established; suppress data.
     * @retval  false  DAQ communication is established; data may be transmitted.
     * @pre     None.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: On_Request.
     * @requirements SWR-REQ-01-04-001;SWR-REQ-01-04-002
     * @see     SendContinualShortDataCyclic100ms, SendContinualLongDataCyclic1000ms
     */
    bool SuppressDataBeforeDaqEstablished() const noexcept;

    /**
     * @brief   Checks if the data upload request flag is non-zero to determine valid trigger.
     * @details Evaluates whether the provided flag value is non-zero, indicating a valid
     *          upload request from ZAT.
     * @param[in] flag  Upload request flag value from ZAT trigger data.
     *                  Valid range: [0, 255]; non-zero indicates a valid request.
     * @return  bool — true if flag is non-zero (valid trigger), false otherwise.
     * @retval  true   Flag is non-zero; trigger is valid.
     * @retval  false  Flag is zero; trigger is invalid and should be ignored.
     * @pre     None.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Cyclic_100ms.
     * @requirements SWR-REQ-03-02-002;SWR-REQ-03-003
     * @see     HandleAdasAcoreTriggerEvt
     */
    bool ValidateTriggerUploadRequestFlag(uint8_t flag) const noexcept;

    /**
     * @brief   Validates whether the acceptance counter for a category has reached the configurable limit.
     * @details Looks up the category in categoryTriggerCountMap_ and compares the count against
     *          sameCategoryLimitPerDC_. Returns true if the limit has been reached.
     * @param[in] categoryId  Category identifier to check. Valid range: uint32_t full range.
     * @return  bool — true if category limit is reached, false if further triggers are allowed.
     * @retval  true   Category acceptance count has reached sameCategoryLimitPerDC_.
     * @retval  false  Category acceptance count is below the limit.
     * @pre     None.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Cyclic_100ms.
     * @requirements SWR-REQ-03-12-001;SWR-REQ-03-12-002;SWR-REQ-03-12-005;SWR-REQ-03-12-006
     * @see     HandleAdasAcoreTriggerEvt, ResetCategoryCounterOnNewDriveCycle
     */
    bool CheckSameCategoryLimit(uint32_t categoryId) const noexcept;

    /**
     * @brief   Validates whether the active event process count exceeds the allowed overlap limit.
     * @details Compares activeEventProcessCount_ against allowedOverlapTriggerNum_ and returns
     *          true if the overlap limit has been exceeded.
     * @return  bool — true if overlap limit is exceeded, false if further overlap is allowed.
     * @retval  true   activeEventProcessCount_ >= allowedOverlapTriggerNum_.
     * @retval  false  activeEventProcessCount_ < allowedOverlapTriggerNum_.
     * @pre     None.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-04-003;SWR-REQ-03-04-004
     * @see     HandleAdasAcoreTriggerEvt, SendAllowedOverlapCountToZAT
     */
    bool CheckOverlapTriggerLimit() const noexcept;

    /**
     * @brief   Clamps log data start time to the negative of past data retention setting when exceeded.
     * @details If startTime is less than the negative of retentionLimit, clamps it to
     *          -static_cast<int32_t>(retentionLimit). Otherwise returns startTime unchanged.
     * @param[in] startTime       Log data start time in seconds. Valid range: int32_t full range.
     * @param[in] retentionLimit  Past data retention limit in seconds. Valid range: [0, INT32_MAX].
     * @return  int32_t — clamped or original start time value.
     * @retval  -static_cast<int32_t>(retentionLimit)  When startTime exceeds the retention limit.
     * @retval  startTime                               When startTime is within the retention limit.
     * @pre     retentionLimit <= static_cast<uint32_t>(INT32_MAX).
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-02-003;SWR-REQ-03-17-007
     * @see     ValidateLogEndTimeClamping
     */
    int32_t ValidateLogStartTimeClamping(int32_t startTime, uint32_t retentionLimit) const noexcept;

    /**
     * @brief   Clamps log data end time to +83 when the received value exceeds the limit.
     * @details If endTime exceeds kMaxLogEndTimeSec (+83), returns kMaxLogEndTimeSec.
     *          Otherwise returns endTime unchanged.
     * @param[in] endTime  Log data end time in seconds. Valid range: int32_t full range.
     * @return  int32_t — clamped or original end time value.
     * @retval  kMaxLogEndTimeSec  When endTime > kMaxLogEndTimeSec.
     * @retval  endTime            When endTime <= kMaxLogEndTimeSec.
     * @pre     None.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-02-004
     * @see     ValidateLogStartTimeClamping
     */
    int32_t ValidateLogEndTimeClamping(int32_t endTime) noexcept;

    /**
     * @brief   Receives 14 JPEG images covering -5 to +8 seconds around the event with timeout enforcement.
     * @details Manages the image reception state machine, checking receivedImageCount_ against
     *          kExpectedImageCount and enforcing EnforceImageAcquisitionTimeout on each cycle.
     *          Sets imageAcquisitionComplete_ when all images are received.
     * @pre     cameraAnswerBackReceived_ is true. imageTimeoutTimerStarted_ is true.
     * @post    imageAcquisitionComplete_ is set to true when all 14 images are received.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-06-006;SWR-REQ-03-06-010
     * @see     HandleCam2ProbePictureEvt, EnforceImageAcquisitionTimeout
     */
    void ReceiveCameraImages() noexcept;

    /**
     * @brief   Evaluates acceptance status from CameraHost answer back message.
     * @details Parses the answerBack byte and returns true if the status indicates acceptance.
     * @param[in] answerBack  Answer back status byte from CameraHost.
     *                        Valid range: [0, 255]; protocol-defined acceptance values.
     * @return  bool — true if CameraHost accepted the trigger, false otherwise.
     * @retval  true   CameraHost accepted the image acquisition trigger.
     * @retval  false  CameraHost rejected the trigger or returned an error status.
     * @pre     None.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-06-002;SWR-REQ-03-06-003
     * @see     HandleCam2ProbeAnswerBackEvt
     */
    bool ValidateCameraAnswerBack(uint8_t answerBack) noexcept;

    /**
     * @brief   Processes trigger-stage and per-image error codes from CameraHost.
     * @details Evaluates the errorCode and takes appropriate action: logs the error,
     *          updates internal state, and may initiate abort procedures.
     * @param[in] errorCode  Error code received from CameraHost.
     *                       Valid range: uint16_t; protocol-defined error values.
     * @pre     None.
     * @post    Internal error state is updated. Abort may be initiated.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-06-004;SWR-REQ-03-06-005
     * @see     HandleCam2ProbeAnswerBackEvt, AbortImageProcessingBeforeTransmitPhase
     */
    void HandleCameraErrorCode(uint16_t errorCode) noexcept;

    /**
     * @brief   Enforces configurable 55-second timeout for image acquisition from CameraHost.
     * @details Checks whether the elapsed time since imageTimeoutTimerStarted_ was set exceeds
     *          imageAcquisitionTimeoutSec_. Returns true if timeout has expired.
     * @return  bool — true if timeout has expired, false if still within timeout window.
     * @retval  true   Image acquisition timeout has expired.
     * @retval  false  Image acquisition is still within the allowed timeout window.
     * @pre     imageTimeoutTimerStarted_ is true.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-06-010
     * @see     AbortImageAcquisitionOnTimeout, ReceiveCameraImages
     */
    bool EnforceImageAcquisitionTimeout() const noexcept;

    /**
     * @brief   Abandons image acquisition and image transmission processing when timeout expires.
     * @details Resets imageAcquisitionComplete_, receivedImageCount_, and imageTimeoutTimerStarted_.
     *          Clears the image buffer and marks the image acquisition as failed.
     * @pre     EnforceImageAcquisitionTimeout returned true.
     * @post    Image acquisition state is reset. Image transmission is skipped.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-06-010
     * @see     EnforceImageAcquisitionTimeout, BuildDummyImageDataForGedr
     */
    void AbortImageAcquisitionOnTimeout() noexcept;

    /**
     * @brief   Abandons image acquisition if answer back not received before image transmission phase.
     * @details Resets cameraAnswerBackReceived_ and imageAcquisitionComplete_ and clears
     *          the image buffer when the answer back was not received in time.
     * @pre     None.
     * @post    Image acquisition state is reset. Image transmission is skipped.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-06-003
     * @see     HandleCam2ProbeAnswerBackEvt
     */
    void AbortImageProcessingBeforeTransmitPhase() noexcept;

    /**
     * @brief   Rounds up PictureSize to multiple of 4 bytes for payload assembly.
     * @details Computes the aligned size as ((pictureSize + 3U) / 4U) * 4U and copies
     *          imageData into a new vector padded to that size with zero bytes.
     * @param[in] pictureSize  Original picture size in bytes. Valid range: [0, 65535].
     * @param[in] imageData    Raw image data bytes. Size must be >= pictureSize.
     * @return  std::vector<uint8_t> — image data padded to the next 4-byte boundary.
     * @pre     imageData.size() >= static_cast<size_t>(pictureSize).
     * @post    Returned vector size is a multiple of 4.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-06-007
     * @see     BuildPayloadImageDataSet
     */
    std::vector<uint8_t> CopyImageDataAligned4Bytes(uint16_t pictureSize,
                                                     std::vector<uint8_t> imageData) const noexcept;

    /**
     * @brief   Builds header with timestamp, category optional data, and event list payload.
     * @details Constructs the event data type 001 header containing the current timestamp,
     *          category identifier, and event list. Appends the category optional data and
     *          event list payload.
     * @param[in] categoryId  Category identifier for the event. Valid range: uint32_t full range.
     * @param[in] transNum    Data transmission number for this event. Valid range: [0, 255].
     * @return  std::vector<uint8_t> — serialized header for event data type 001.
     * @pre     currentCategoryIdentifier_ and currentDataTransmissionNum_ are set.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-07-003;SWR-REQ-03-07-010
     * @see     BuildPayloadCategoryOptionalAndEventList
     */
    std::vector<uint8_t> FillHeaderEventDataType001(uint32_t categoryId,
                                                     uint8_t transNum) const noexcept;

    /**
     * @brief   Builds header with timestamp and log data set payload.
     * @details Constructs the event data type 002 header containing the current timestamp
     *          and appends the provided log data set.
     * @param[in] logDataSet  Serialized log data set payload. Must not be empty.
     * @return  std::vector<uint8_t> — serialized header for event data type 002.
     * @pre     None.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-07-004
     * @see     BuildPayloadLogDataSet
     */
    std::vector<uint8_t> FillHeaderEventDataType002(std::vector<uint8_t> logDataSet) const noexcept;

    /**
     * @brief   Builds header with timestamp and 14 image frames payload.
     * @details Constructs the event data type 003 header containing the current timestamp
     *          and appends the provided image data.
     * @param[in] imageData  Serialized image data payload containing 14 frames. Must not be empty.
     * @return  std::vector<uint8_t> — serialized header for event data type 003.
     * @pre     None.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-07-005
     * @see     BuildPayloadImageDataSet
     */
    std::vector<uint8_t> FillHeaderEventDataType003(std::vector<uint8_t> imageData) const noexcept;

    /**
     * @brief   Constructs the first event payload with category data and event list.
     * @details Serializes the category optional data and event list from the trigger data
     *          into the first event payload structure.
     * @param[in] triggerData  Serialized ZAT trigger data. Must not be empty.
     * @return  std::vector<uint8_t> — serialized category optional data and event list payload.
     * @pre     triggerData is non-empty and conforms to ZAT trigger format.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-07-001;SWR-REQ-03-07-003
     * @see     FillHeaderEventDataType001
     */
    std::vector<uint8_t> BuildPayloadCategoryOptionalAndEventList(
        std::vector<uint8_t> triggerData) const noexcept;

    /**
     * @brief   Constructs the second event payload with log data set in chronological order.
     * @details Serializes the log data into the second event payload structure, ensuring
     *          chronological ordering of log entries.
     * @param[in] logData  Serialized log data. Must not be empty.
     * @return  std::vector<uint8_t> — serialized log data set payload in chronological order.
     * @pre     logData is non-empty.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-07-004;SWR-REQ-03-07-006
     * @see     FillHeaderEventDataType002
     */
    std::vector<uint8_t> BuildPayloadLogDataSet(std::vector<uint8_t> logData) const noexcept;

    /**
     * @brief   Constructs the third event payload with 14 image frames and timestamps.
     * @details Serializes the image data into the third event payload structure, including
     *          per-frame timestamps.
     * @param[in] imageData  Serialized image data containing 14 frames. Must not be empty.
     * @return  std::vector<uint8_t> — serialized image data set payload.
     * @pre     imageData is non-empty. receivedImageCount_ == kExpectedImageCount.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-07-005
     * @see     FillHeaderEventDataType003
     */
    std::vector<uint8_t> BuildPayloadImageDataSet(std::vector<uint8_t> imageData) const noexcept;

    /**
     * @brief   Divides event data into numbered segments when total size exceeds max packet size.
     * @details Splits the payload into segments of the maximum allowed packet size, assigns
     *          segment numbers, and sets the data information flag for each segment.
     * @param[in] payload  Complete event data payload to be segmented. Must not be empty.
     * @return  std::vector<std::vector<uint8_t>> — ordered list of payload segments.
     * @pre     payload is non-empty.
     * @post    segmentNumber_ and totalSegments_ are updated.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-08-001;SWR-REQ-03-08-002;SWR-REQ-03-08-003;SWR-REQ-03-07-008
     * @see     SetDataInformationFlag
     */
    std::vector<std::vector<uint8_t>> SegmentPayloadIfExceedsMaxSize(
        std::vector<uint8_t> payload) noexcept;

    /**
     * @brief   Marks each segment with start, middle, end, or single-packet indicator.
     * @details Computes the data information flag byte based on the combination of
     *          isSinglePacket, isStart, and isEnd flags.
     * @param[in] isSinglePacket  True if the payload fits in a single packet.
     * @param[in] isStart         True if this is the first segment of a multi-segment payload.
     * @param[in] isEnd           True if this is the last segment of a multi-segment payload.
     * @return  uint8_t — data information flag byte encoding the segment position.
     * @retval  0x00  Single packet (isSinglePacket == true).
     * @retval  0x01  Start segment.
     * @retval  0x02  Middle segment.
     * @retval  0x03  End segment.
     * @pre     Exactly one of the flag combinations is valid per call.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-08-002;SWR-REQ-03-08-003
     * @see     SegmentPayloadIfExceedsMaxSize
     */
    uint8_t SetDataInformationFlag(bool isSinglePacket, bool isStart, bool isEnd) const noexcept;

    /**
     * @brief   Sends transmission permission request to DAQ before sending event data.
     * @details Constructs and transmits the REQUEST_BDP_UPLOAD_REQ message to DAQ with the
     *          provided parameters. Returns true if DAQ grants permission.
     * @param[in] clientId    DAQ client identifier. Valid range: uint16_t full range.
     * @param[in] eventValue  Event value for the upload request. Valid range: uint32_t full range.
     * @param[in] dataSize    Total data size in bytes. Valid range: uint32_t full range.
     * @param[in] requestNum  Upload request number. Valid range: uint32_t full range.
     * @return  bool — true if DAQ granted upload permission, false otherwise.
     * @retval  true   DAQ responded with REQUEST_BDP_UPLOAD_REQ_RES indicating permission granted.
     * @retval  false  DAQ denied permission or no response received within timeout.
     * @pre     daqCommunicationEstablished_ is true.
     * @post    bdpRequestRetryCounter_ may be incremented on failure.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-09-001;SWR-REQ-03-09-002
     * @see     HandleBdpRequestResponseStatusCode, EnforceBdpRequestRetry
     */
    bool SendBdpUploadRequestToDAQ(uint16_t clientId, uint32_t eventValue,
                                   uint32_t dataSize, uint32_t requestNum) noexcept;

    /**
     * @brief   Transmits event data payload to DAQ via SOME/IP TCP BDP protocol.
     * @details Constructs and transmits the TRANSMIT_BDP_REQ message to DAQ with the provided
     *          payload. Returns true if DAQ acknowledges successful reception.
     * @param[in] clientId   DAQ client identifier. Valid range: uint16_t full range.
     * @param[in] requestNum Upload request number matching the prior SendBdpUploadRequestToDAQ call.
     * @param[in] contents   Serialized event data payload. Must not be empty.
     * @return  bool — true if DAQ acknowledged successful reception, false otherwise.
     * @retval  true   DAQ responded with TRANSMIT_BDP_REQ_RES indicating success.
     * @retval  false  DAQ returned an error or no response received within timeout.
     * @pre     SendBdpUploadRequestToDAQ returned true for the same requestNum.
     * @post    bdpTransmitRetryCounter_ may be incremented on failure.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-07-011;SWR-REQ-03-09-002
     * @see     HandleBdpTransmitReplyStatusCode, EnforceBdpTransmitRetry
     */
    bool TransmitBdpDataToDAQ(uint16_t clientId, uint32_t requestNum,
                               std::vector<uint8_t> contents) noexcept;

    /**
     * @brief   Evaluates DAQ response to upload request and triggers retry or proceed.
     * @details Parses the statusCode from the DAQ upload request response and returns true
     *          if the status indicates permission granted.
     * @param[in] statusCode  Status code from DAQ REQUEST_BDP_UPLOAD_REQ_RES.
     *                        Valid range: uint16_t; protocol-defined status values.
     * @return  bool — true if status indicates permission granted, false otherwise.
     * @retval  true   Status code indicates DAQ granted upload permission.
     * @retval  false  Status code indicates denial or error; retry or abort required.
     * @pre     None.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-09-002;SWR-REQ-03-09-003
     * @see     SendBdpUploadRequestToDAQ, EnforceBdpRequestRetry
     */
    bool HandleBdpRequestResponseStatusCode(uint16_t statusCode) const noexcept;

    /**
     * @brief   Evaluates DAQ reply after data transmission.
     * @details Parses the statusCode from the DAQ TRANSMIT_BDP_REQ_RES and returns true
     *          if the status indicates successful reception.
     * @param[in] statusCode  Status code from DAQ TRANSMIT_BDP_REQ_RES.
     *                        Valid range: uint16_t; protocol-defined status values.
     * @return  bool — true if status indicates successful transmission, false otherwise.
     * @retval  true   Status code indicates DAQ received the data successfully.
     * @retval  false  Status code indicates error; retry or abort required.
     * @pre     None.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-09-002;SWR-REQ-03-11-002
     * @see     TransmitBdpDataToDAQ, EnforceBdpTransmitRetry
     */
    bool HandleBdpTransmitReplyStatusCode(uint16_t statusCode) const noexcept;

    /**
     * @brief   Retries upload request after configurable wait time up to maximum retry count.
     * @details Checks bdpRequestRetryCounter_ against bdpMaxRetryCount_. If below limit,
     *          increments the counter and schedules a retry after bdpRetryIntervalMs_.
     *          Returns true if retry is possible, false if max retries are exhausted.
     * @return  bool — true if retry was scheduled, false if max retries are exhausted.
     * @retval  true   Retry scheduled; bdpRequestRetryCounter_ incremented.
     * @retval  false  bdpRequestRetryCounter_ >= bdpMaxRetryCount_; abort required.
     * @pre     None.
     * @post    bdpRequestRetryCounter_ may be incremented.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-09-003;SWR-REQ-03-11-001
     * @see     AbortEventDataTransmissionOnRetryExhaustion
     */
    bool EnforceBdpRequestRetry() noexcept;

    /**
     * @brief   Retries data transmission after configurable wait time up to maximum retransmission count.
     * @details Checks bdpTransmitRetryCounter_ against bdpMaxRetryCount_. If below limit,
     *          increments the counter and schedules a retransmission after bdpRetryIntervalMs_.
     *          Returns true if retry is possible, false if max retries are exhausted.
     * @return  bool — true if retry was scheduled, false if max retries are exhausted.
     * @retval  true   Retry scheduled; bdpTransmitRetryCounter_ incremented.
     * @retval  false  bdpTransmitRetryCounter_ >= bdpMaxRetryCount_; abort required.
     * @pre     None.
     * @post    bdpTransmitRetryCounter_ may be incremented.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-11-002;SWR-REQ-03-11-005
     * @see     AbortEventDataTransmissionOnRetryExhaustion
     */
    bool EnforceBdpTransmitRetry() noexcept;

    /**
     * @brief   Resets all retry counters to zero after successful transmission cycle.
     * @details Sets bdpRequestRetryCounter_, bdpTransmitRetryCounter_, and
     *          gedrWriteRetryCounter_ to zero.
     * @pre     None.
     * @post    All retry counters are zero.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-11-006
     * @see     EnforceBdpRequestRetry, EnforceBdpTransmitRetry
     */
    void ResetRetryCountersOnSuccess() noexcept;

    /**
     * @brief   Stops all DAQ transmission and discards data when max retries are exhausted.
     * @details Calls DeleteEventDataOnRetryExhaustion, resets all retry counters, and
     *          decrements activeEventProcessCount_.
     * @pre     EnforceBdpRequestRetry or EnforceBdpTransmitRetry returned false.
     * @post    Event data is discarded. activeEventProcessCount_ is decremented.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-09-004;SWR-REQ-03-11-003;SWR-REQ-03-11-004
     * @see     DeleteEventDataOnRetryExhaustion, ResetRetryCountersOnSuccess
     */
    void AbortEventDataTransmissionOnRetryExhaustion() noexcept;

    /**
     * @brief   Ensures event data is transmitted in strict order: header+category, log data, image data.
     * @details Manages the eventDataFrameCounter_ state machine to enforce the mandatory
     *          transmission ordering of the three event data types.
     * @pre     Event data payloads are ready for transmission.
     * @post    eventDataFrameCounter_ is advanced to the next transmission phase.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-07-001;SWR-REQ-03-04-002
     * @see     SendAdasCategoryDataToDAQ, SendLogDataSetsToDAQ, SendImageDataSetToDAQ
     */
    void EnforceEventDataTransmissionOrder() noexcept;

    /**
     * @brief   Transmits header with category optional data and event list to DAQ.
     * @details Calls SendBdpUploadRequestToDAQ and TransmitBdpDataToDAQ with the category
     *          payload. On success, calls SendAdasCategoryDataToGEDR.
     * @param[in] payload  Serialized category optional data and event list payload. Must not be empty.
     * @pre     daqCommunicationEstablished_ is true.
     * @post    Category data is transmitted to DAQ. GEDR transmission is initiated.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-18-002;SWR-REQ-03-07-003
     * @see     SendAdasCategoryDataToGEDR
     */
    void SendAdasCategoryDataToDAQ(std::vector<uint8_t> payload) noexcept;

    /**
     * @brief   Transmits log data sets to DAQ in chronological order without waiting for images.
     * @details Iterates over logPayloads and calls SendBdpUploadRequestToDAQ and
     *          TransmitBdpDataToDAQ for each. On success, calls SendLogDataSetsToGEDR.
     * @param[in] logPayloads  Ordered list of serialized log data set payloads. Must not be empty.
     * @pre     daqCommunicationEstablished_ is true.
     * @post    All log data sets are transmitted to DAQ. GEDR transmission is initiated.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-07-004;SWR-REQ-03-07-006;SWR-REQ-03-07-012;SWR-REQ-03-18-004
     * @see     SendLogDataSetsToGEDR
     */
    void SendLogDataSetsToDAQ(std::vector<std::vector<uint8_t>> logPayloads) noexcept;

    /**
     * @brief   Transmits 14 image frames with timestamps to DAQ as soon as image data is received.
     * @details Calls SendBdpUploadRequestToDAQ and TransmitBdpDataToDAQ with the image payload.
     *          On success, calls SendImageDataSetToGEDR.
     * @param[in] imagePayload  Serialized image data set payload containing 14 frames. Must not be empty.
     * @pre     daqCommunicationEstablished_ is true. imageAcquisitionComplete_ is true.
     * @post    Image data set is transmitted to DAQ. GEDR transmission is initiated.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-07-005;SWR-REQ-03-18-006
     * @see     SendImageDataSetToGEDR
     */
    void SendImageDataSetToDAQ(std::vector<uint8_t> imagePayload) noexcept;

    /**
     * @brief   Transmits category data to GEDR API after DAQ transmission.
     * @details Calls UploadGedrWrite with the category payload after DAQ transmission
     *          of the same data has completed successfully.
     * @param[in] payload  Serialized category optional data and event list payload. Must not be empty.
     * @pre     SendAdasCategoryDataToDAQ completed successfully.
     * @post    Category data is written to GEDR.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-17-001;SWR-REQ-03-18-003
     * @see     UploadGedrWrite
     */
    void SendAdasCategoryDataToGEDR(std::vector<uint8_t> payload) noexcept;

    /**
     * @brief   Transmits log data in 1-second units to GEDR API.
     * @details Iterates over logPayloads and calls UploadGedrWrite for each 1-second unit.
     * @param[in] logPayloads  Ordered list of serialized log data set payloads. Must not be empty.
     * @pre     SendLogDataSetsToDAQ completed successfully.
     * @post    All log data sets are written to GEDR.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-17-002;SWR-REQ-03-18-005
     * @see     UploadGedrWrite
     */
    void SendLogDataSetsToGEDR(std::vector<std::vector<uint8_t>> logPayloads) noexcept;

    /**
     * @brief   Transmits 14 image data set to GEDR API.
     * @details Calls UploadGedrWrite with the image payload.
     * @param[in] imagePayload  Serialized image data set payload containing 14 frames. Must not be empty.
     * @pre     SendImageDataSetToDAQ completed successfully or dummy data is used.
     * @post    Image data set is written to GEDR.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-17-002;SWR-REQ-03-18-007
     * @see     UploadGedrWrite, BuildDummyImageDataForGedr
     */
    void SendImageDataSetToGEDR(std::vector<uint8_t> imagePayload) noexcept;

    /**
     * @brief   Calls GEDR_Write with offset, data, data_size, start_block, and end_block parameters.
     * @details Invokes the GEDR write API with the provided parameters. Returns the GEDR_Write
     *          return code for caller evaluation.
     * @param[in] offset     Write offset in bytes. Valid range: uint32_t full range.
     * @param[in] data       Data bytes to write. Must not be empty.
     * @param[in] dataSize   Number of bytes to write. Must equal data.size().
     * @param[in] startBlock True if this is the start of a GEDR block.
     * @param[in] endBlock   True if this is the end of a GEDR block.
     * @return  int8_t — GEDR_Write return code.
     * @retval  0   GEDR_Write succeeded.
     * @retval  <0  GEDR_Write returned a retryable error code.
     * @retval  >0  GEDR_Write returned a fatal error code.
     * @pre     data.size() == static_cast<size_t>(dataSize).
     * @post    Data is written to GEDR on success.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-17-001;SWR-REQ-03-17-003;SWR-REQ-03-17-004;SWR-REQ-03-17-005
     * @see     HandleGedrRetryOnError, HandleGedrFatalError
     */
    int8_t UploadGedrWrite(uint32_t offset, std::vector<uint8_t> data,
                           uint32_t dataSize, bool startBlock, bool endBlock) noexcept;

    /**
     * @brief   Retries GEDR write on error with bounded retry count.
     * @details Checks gedrWriteRetryCounter_ against gedrMaxRetryCount_. If below limit,
     *          increments the counter and returns true to indicate retry is possible.
     * @param[in] returnCode  Return code from UploadGedrWrite. Valid range: int8_t full range.
     * @return  bool — true if retry is possible, false if max retries are exhausted.
     * @retval  true   gedrWriteRetryCounter_ < gedrMaxRetryCount_; retry scheduled.
     * @retval  false  gedrWriteRetryCounter_ >= gedrMaxRetryCount_; abort required.
     * @pre     None.
     * @post    gedrWriteRetryCounter_ may be incremented.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-17-001
     * @see     HandleGedrFatalError
     */
    bool HandleGedrRetryOnError(int8_t returnCode) noexcept;

    /**
     * @brief   Acts on fatal GEDR API error codes by aborting GEDR transmission.
     * @details Evaluates returnCode for fatal error conditions and aborts GEDR transmission,
     *          resetting gedrWriteRetryCounter_ and marking GEDR transmission as failed.
     * @param[in] returnCode  Return code from UploadGedrWrite indicating a fatal error.
     *                        Valid range: int8_t; positive values indicate fatal errors.
     * @pre     None.
     * @post    GEDR transmission is aborted. gedrWriteRetryCounter_ is reset.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-17-001
     * @see     HandleGedrRetryOnError
     */
    void HandleGedrFatalError(int8_t returnCode) noexcept;

    /**
     * @brief   Creates header-only dummy data with length 0 when camera image collection fails.
     * @details Constructs a minimal event data type 003 header with image data length set to 0,
     *          used as a placeholder when image acquisition failed or timed out.
     * @return  std::vector<uint8_t> — serialized dummy image data header with zero-length payload.
     * @pre     imageAcquisitionComplete_ is false or AbortImageAcquisitionOnTimeout was called.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-17-006
     * @see     SendImageDataSetToGEDR
     */
    std::vector<uint8_t> BuildDummyImageDataForGedr() const noexcept;

    /**
     * @brief   Clamps GEDR log start time to past data retention limit.
     * @details If startTime is greater than -1 (i.e., not in the past), suppresses the data.
     *          Otherwise clamps to the retention limit per SWR-REQ-03-17-007.
     * @param[in] startTime  GEDR log start time in seconds. Valid range: int32_t full range.
     * @return  int32_t — clamped GEDR log start time.
     * @pre     None.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-17-007
     * @see     SuppressGedrDataWhenStartTimeInvalid
     */
    int32_t ValidateGedrLogStartTime(int32_t startTime) const noexcept;

    /**
     * @brief   Clamps GEDR log data end time to -1 when greater than -1.
     * @details If endTime > kGedrMaxLogEndTimeSec (-1), returns kGedrMaxLogEndTimeSec.
     *          Otherwise returns endTime unchanged.
     * @param[in] endTime  GEDR log end time in seconds. Valid range: int32_t full range.
     * @return  int32_t — clamped GEDR log end time.
     * @retval  kGedrMaxLogEndTimeSec  When endTime > -1.
     * @retval  endTime                When endTime <= -1.
     * @pre     None.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-17-008
     * @see     ValidateGedrLogStartTime
     */
    int32_t ValidateGedrLogEndTime(int32_t endTime) const noexcept;

    /**
     * @brief   Does not send log data to GEDR API when log data start time is greater than -1.
     * @details Returns true (suppress) when startTime > -1, indicating the log data start
     *          time is invalid for GEDR transmission.
     * @param[in] startTime  GEDR log start time in seconds. Valid range: int32_t full range.
     * @return  bool — true if data should be suppressed, false if transmission is allowed.
     * @retval  true   startTime > -1; suppress GEDR log data transmission.
     * @retval  false  startTime <= -1; GEDR log data transmission is allowed.
     * @pre     None.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-17-009
     * @see     ValidateGedrLogStartTime
     */
    bool SuppressGedrDataWhenStartTimeInvalid(int32_t startTime) const noexcept;

    /**
     * @brief   Ensures correct DAQ-before-GEDR ordering for both probe and GEDR triggers.
     * @details Manages the dual-trigger transmission state machine, enforcing that DAQ
     *          transmission always precedes GEDR transmission for each event data type.
     * @pre     Event data payloads are ready for transmission.
     * @post    Transmission ordering state is advanced correctly.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-18-001;SWR-REQ-03-18-002;SWR-REQ-03-18-003;SWR-REQ-03-18-004;SWR-REQ-03-18-005
     * @see     EnforceEventDataTransmissionOrder
     */
    void EnforceDualTriggerTransmissionOrder() noexcept;

    /**
     * @brief   Indicates trigger rejection when maximum duplicates exceeded.
     * @details Constructs and transmits the ADAS_ACore_PROBE2ZAT_EVT rejection message
     *          via the probeEventStt SOME/IP service.
     * @pre     OfferServiceProbeEventStt has been called. ZAT has subscribed.
     * @post    ADAS_ACore_PROBE2ZAT_EVT rejection is transmitted to ZAT.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Cyclic_100ms.
     * @requirements SWR-REQ-03-05-002
     * @see     SendTriggerAcceptanceStatusToZAT
     */
    void SendTriggerRejectionToZAT() noexcept;

    /**
     * @brief   Removes development event data from memory after successful DAQ transmission confirmation.
     * @details Clears the internal event data buffers after HandleResultBdpEvt confirms
     *          successful DAQ upload. Decrements activeEventProcessCount_ if all targets complete.
     * @pre     HandleResultBdpEvt confirmed successful DAQ upload.
     * @post    DAQ event data buffers are cleared.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-10-001
     * @see     CheckRetainDataUntilAllTargetsComplete
     */
    void DeleteEventDataAfterDaqSuccess() noexcept;

    /**
     * @brief   Removes development event data from memory after successful GEDR transmission.
     * @details Clears the internal event data buffers after GEDR write confirms successful
     *          transmission. Decrements activeEventProcessCount_ if all targets complete.
     * @pre     UploadGedrWrite completed successfully for all event data.
     * @post    GEDR event data buffers are cleared.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-10-002
     * @see     CheckRetainDataUntilAllTargetsComplete
     */
    void DeleteEventDataAfterGedrSuccess() noexcept;

    /**
     * @brief   Discards event data when maximum retries are exhausted.
     * @details Clears all internal event data buffers and resets transmission state
     *          when retry exhaustion is detected.
     * @pre     AbortEventDataTransmissionOnRetryExhaustion was called.
     * @post    All event data buffers are cleared. Transmission state is reset.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-09-004;SWR-REQ-03-11-004
     * @see     AbortEventDataTransmissionOnRetryExhaustion
     */
    void DeleteEventDataOnRetryExhaustion() noexcept;

    /**
     * @brief   Retains event data until transmission to all designated destinations is confirmed successful.
     * @details Checks whether both DAQ and GEDR transmissions have completed successfully.
     *          Returns true if data should still be retained (not all targets complete).
     * @return  bool — true if data must be retained, false if all targets have confirmed success.
     * @retval  true   One or more transmission targets have not yet confirmed success.
     * @retval  false  All designated transmission targets have confirmed successful reception.
     * @pre     None.
     * @post    No state change.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: Event_Trigger.
     * @requirements SWR-REQ-03-10-003
     * @see     DeleteEventDataAfterDaqSuccess, DeleteEventDataAfterGedrSuccess
     */
    bool CheckRetainDataUntilAllTargetsComplete() const noexcept;

    /**
     * @brief   Writes data sent to DAQ into log file with _snd_YYYYMMDD-HHMMSS.log naming.
     * @details If devLogEnabled_ is true, serializes the data vector and appends it to the
     *          current send log file. File name format: <prefix>_snd_YYYYMMDD-HHMMSS.log.
     * @param[in] data  Data bytes that were sent to DAQ. Must not be empty.
     * @pre     devLogEnabled_ is true.
     * @post    Data is appended to the send log file.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: On_Request.
     * @requirements SWR-REQ-01-11-001;SWR-REQ-01-11-002;SWR-REQ-03-15-001
     * @see     EnableDevLogFileOutput, WriteDevLogReceivedData
     */
    void WriteDevLogSentData(std::vector<uint8_t> data) noexcept;

    /**
     * @brief   Writes data received from DAQ into log file with _rcv_YYYYMMDD-HHMMSS.log naming.
     * @details If devLogEnabled_ is true, serializes the data vector and appends it to the
     *          current receive log file. File name format: <prefix>_rcv_YYYYMMDD-HHMMSS.log.
     * @param[in] data  Data bytes that were received from DAQ. Must not be empty.
     * @pre     devLogEnabled_ is true.
     * @post    Data is appended to the receive log file.
     * @throws  None.
     * @note    Called by: ProbeComm. Call condition: On_Request.
     * @requirements SWR-REQ-03-15-002
     * @see     EnableDevLogFileOutput, WriteDevLogSentData
     */
    void WriteDevLogReceivedData(std::vector<uint8_t> data) noexcept;

    // =========================================================================
    // Private Member Variables
    // =========================================================================

    ProbeCommVariant* variant_{nullptr};  ///< @brief Non-owning pointer to variant configuration. Must not be nullptr after construction.

    bool daqCommunicationEstablished_{false};  ///< @brief True when SOME/IP DAQ service discovery has completed and communication is active. Range: {true, false}.

    bool zatTriggerReceived_{false};  ///< @brief True when a valid ZAT trigger has been received in the current cycle. Range: {true, false}.

    bool cameraAnswerBackReceived_{false};  ///< @brief True when CameraHost has responded to the image acquisition trigger with an acceptance. Range: {true, false}.

    bool imageAcquisitionComplete_{false};  ///< @brief True when all kExpectedImageCount images have been received from CameraHost. Range: {true, false}.

    uint32_t bdpRequestRetryCounter_{0U};  ///< @brief Current BDP upload request retry count. Range: [0, bdpMaxRetryCount_].

    uint32_t bdpTransmitRetryCounter_{0U};  ///< @brief Current BDP data transmit retry count. Range: [0, bdpMaxRetryCount_].

    uint32_t gedrWriteRetryCounter_{0U};  ///< @brief Current GEDR write retry count. Range: [0, gedrMaxRetryCount_].

    int32_t activeEventProcessCount_{0};  ///< @brief Number of currently active event processing instances. Range: [0, allowedOverlapTriggerNum_].

    std::map<uint32_t, uint32_t> categoryTriggerCountMap_null;  ///< @brief Map from category ID to acceptance count within the current drive cycle. Key: category ID; Value: acceptance count in range [0, sameCategoryLimitPerDC_].

    uint32_t currentDataTransmissionNum_{0U};  ///< @brief Data transmission number for the current event being processed. Range: uint32_t full range.

    uint32_t currentCategoryIdentifier_{0U};  ///< @brief Category identifier for the current event being processed. Range: uint32_t full range.

    uint8_t dataUploadRequestFlg_{0U};  ///< @brief Upload request flag received from ZAT trigger data. Non-zero indicates a valid upload request. Range: [0, 255].

    int32_t effectiveLogStartTime_{0};  ///< @brief Clamped log data start time in seconds after ValidateLogStartTimeClamping. Range: [-(retentionLimit), 0].

    int32_t effectiveLogEndTime_{0};  ///< @brief Clamped log data end time in seconds after ValidateLogEndTimeClamping. Range: [INT32_MIN, kMaxLogEndTimeSec].

    uint16_t imageFrameCounter_{0U};  ///< @brief Frame counter value from the most recently received camera image. Range: [0, 65535].

    uint8_t receivedImageCount_{0U};  ///< @brief Number of camera images received in the current acquisition cycle. Range: [0, kExpectedImageCount].

    uint32_t eventDataFrameCounter_{0U};  ///< @brief State counter tracking the current phase of event data transmission ordering. Range: [0, 2] corresponding to category, log, image phases.

    uint32_t segmentNumber_{0U};  ///< @brief Current segment number within a segmented payload. Range: [0, totalSegments_].

    uint32_t totalSegments_{0U};  ///< @brief Total number of segments for the current payload. Range: [1, UINT32_MAX].

    uint32_t acceptedDataTransmissionNum_{0U};  ///< @brief Data transmission number of the most recently accepted event, reported to ZAT. Range: uint32_t full range.

    uint32_t acceptedCategoryIdentifier_{0U};  ///< @brief Category identifier of the most recently accepted event, reported to ZAT. Range: uint32_t full range.

    int32_t allowedOverlapTriggerNum_{0};  ///< @brief Maximum number of concurrently active event processes allowed. Range: [0, INT32_MAX].

    uint32_t imageAcquisitionTimeoutSec_{kDefaultImageAcquisitionTimeoutSec};  ///< @brief Configurable image acquisition timeout in seconds. Default: kDefaultImageAcquisitionTimeoutSec (55). Range: [1, UINT32_MAX].

    uint32_t bdpRetryIntervalMs_{0U};  ///< @brief Wait interval in milliseconds between BDP request/transmit retries. Range: [0, UINT32_MAX].

    uint32_t bdpMaxRetryCount_{0U};  ///< @brief Maximum number of BDP request and transmit retry attempts. Range: [0, UINT32_MAX].

    uint32_t gedrMaxRetryCount_{0U};  ///< @brief Maximum number of GEDR write retry attempts. Range: [0, UINT32_MAX].

    uint32_t sameCategoryLimitPerDC_{0U};  ///< @brief Maximum number of accepted triggers per category per drive cycle. Range: [0, UINT32_MAX].

    bool devLogEnabled_{false};  ///< @brief True when development log file output is enabled. Must be false in production builds. Range: {true, false}.

    bool imageTimeoutTimerStarted_{false};  ///< @brief True when the image acquisition timeout timer has been started. Range: {true, false}.

    mutable std::mutex sendBufferMutex_null;  ///< @brief Mutex protecting the DAQ send buffer and daqCommunicationEstablished_ flag from concurrent access.

    mutable std::mutex cameraMutex_null;  ///< @brief Mutex protecting camera image buffer, receivedImageCount_, imageAcquisitionComplete_, and related camera state from concurrent access.
};

}  // namespace probe