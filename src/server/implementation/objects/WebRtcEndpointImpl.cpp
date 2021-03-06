/*
 * (C) Copyright 2016 Kurento (http://kurento.org/)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include <gst/gst.h>
#include "MediaPipeline.hpp"
#include <WebRtcEndpointImplFactory.hpp>
#include "WebRtcEndpointImpl.hpp"
#include <jsonrpc/JsonSerializer.hpp>
#include <KurentoException.hpp>
#include <boost/filesystem.hpp>
#include <IceCandidate.hpp>
#include "IceCandidatePair.hpp"
#include "IceConnection.hpp"
#include "CertificateKeyType.hpp"
#include <webrtcendpoint/kmsicecandidate.h>
#include <IceComponentState.hpp>
#include <SignalHandler.hpp>
#include <webrtcendpoint/kmsicebaseagent.h>

#include <StatsType.hpp>
#include <RTCDataChannelState.hpp>
#include <RTCDataChannelStats.hpp>
#include <RTCPeerConnectionStats.hpp>
#include <commons/kmsstats.h>
#include <commons/kmsutils.h>

#include "webrtcendpoint/kmswebrtcdatachannelstate.h"
#include <boost/algorithm/string.hpp>

#include <CertificateManager.hpp>

#define GST_CAT_DEFAULT kurento_web_rtc_endpoint_impl
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);
#define GST_DEFAULT_NAME "KurentoWebRtcEndpointImpl"

#define FACTORY_NAME "webrtcendpoint"

#define CONFIG_PATH "configPath"
#define DEFAULT_PATH "/etc/kurento"

namespace kurento
{

static const uint DEFAULT_STUN_PORT = 3478;

std::once_flag check_openh264, certificates_flag;
std::string defaultCertificateRSA, defaultCertificateECDSA;
std::vector<std::string> supported_codecs = { "VP8", "opus", "PCMU" };

static void
remove_not_supported_codecs_from_array (GstElement *element, GArray *codecs)
{
  guint i;

  if (codecs == NULL) {
    return;
  }

  for (i = 0; i < codecs->len; i++) {
    GValue *v = &g_array_index (codecs, GValue, i);
    const GstStructure *s;
    const gchar *codec_name;
    gboolean supported = FALSE;

    if (!GST_VALUE_HOLDS_STRUCTURE (v) ) {
      GST_WARNING_OBJECT (element, "Value into array is not a GstStructure");
      continue;
    }

    s = gst_value_get_structure (v);
    codec_name = gst_structure_get_name (s);

    for (std::vector<std::string>::iterator it = supported_codecs.begin();
         it != supported_codecs.end(); ++it) {

      if (boost::istarts_with (codec_name, (*it) ) ) {
        supported = TRUE;
        break;
      }
    }

    if (!supported) {
      GST_INFO_OBJECT (element, "Removing not supported codec '%s'", codec_name);
      g_array_remove_index (codecs, i);
      i--;
    }
  }
}

static void
remove_not_supported_codecs (GstElement *element)
{
  GArray *codecs;

  g_object_get (element, "audio-codecs", &codecs, NULL);
  remove_not_supported_codecs_from_array (element, codecs);
  g_array_unref (codecs);

  g_object_get (element, "video-codecs", &codecs, NULL);
  remove_not_supported_codecs_from_array (element, codecs);
  g_array_unref (codecs);
}

static void
check_support_for_h264 ()
{
  GstPlugin *plugin;

  plugin = gst_plugin_load_by_name ("openh264");

  if (plugin == NULL) {
    return;
  }

  supported_codecs.push_back ("H264");
  gst_object_unref (plugin);
}

void
WebRtcEndpointImpl::generateDefaultCertificates ()
{
  std::string pemUri;
  std::string pemUriRSA;
  std::string pemUriECDSA;

  defaultCertificateECDSA = "";
  defaultCertificateRSA = "";

  try {
    pemUriRSA = getConfigValue <std::string, WebRtcEndpoint> ("pemCertificateRSA");
    defaultCertificateRSA = getCerficateFromFile (pemUriRSA);
  } catch (boost::property_tree::ptree_error &e) {
    try {
      pemUri = getConfigValue <std::string, WebRtcEndpoint> ("pemCertificate");
      GST_WARNING ("pemCertificate is deprecated. Please use pemCertificateRSA instead");
      defaultCertificateRSA = getCerficateFromFile (pemUri);
    } catch (boost::property_tree::ptree_error &e) {
      GST_INFO ("Unable to load the RSA certificate from file. Using the default certificate.");
      defaultCertificateRSA = CertificateManager::generateRSACertificate ();
    }
  }

  try {
    pemUriECDSA = getConfigValue
                  <std::string, WebRtcEndpoint> ("pemCertificateECDSA");
    defaultCertificateECDSA = getCerficateFromFile (pemUriECDSA);
  } catch (boost::property_tree::ptree_error &e) {
    GST_INFO ("Unable to load the ECDSA certificate from file. Using the default certificate.");
    defaultCertificateECDSA = CertificateManager::generateECDSACertificate ();
  }
}

void WebRtcEndpointImpl::checkUri (std::string &uri)
{
  //Check if uri is an absolute or relative path.
  if (! boost::starts_with (uri, "/") ) {
    std::string path;

    try {
      path = getConfigValue <std::string, WebRtcEndpoint> (CONFIG_PATH);
    } catch (boost::property_tree::ptree_error &e) {
      GST_DEBUG ("WebRtcEndpoint config file doesn't contain a defaul path");
      path = getConfigValue <std::string> (CONFIG_PATH, DEFAULT_PATH);
    }

    uri = path + "/" + uri;
  }
}

void WebRtcEndpointImpl::onIceCandidate (gchar *sessId,
    KmsIceCandidate *candidate)
{
  try {
    std::string cand_str (kms_ice_candidate_get_candidate (candidate) );
    std::string mid_str (kms_ice_candidate_get_sdp_mid (candidate) );
    int sdp_m_line_index = kms_ice_candidate_get_sdp_m_line_index (candidate);
    std::shared_ptr <IceCandidate> cand ( new  IceCandidate
                                          (cand_str, mid_str, sdp_m_line_index) );
    OnIceCandidate event (shared_from_this(), OnIceCandidate::getName(), cand);
    IceCandidateFound newEvent (shared_from_this(), IceCandidateFound::getName(),
                                cand);

    signalOnIceCandidate (event);
    signalIceCandidateFound (newEvent);
  } catch (std::bad_weak_ptr &e) {
  }
}

void WebRtcEndpointImpl::onIceGatheringDone (gchar *sessId)
{
  try {
    OnIceGatheringDone event (shared_from_this(), OnIceGatheringDone::getName() );
    IceGatheringDone newEvent (shared_from_this(), IceGatheringDone::getName() );

    signalOnIceGatheringDone (event);
    signalIceGatheringDone (newEvent);
  } catch (std::bad_weak_ptr &e) {
  }
}

void WebRtcEndpointImpl::onIceComponentStateChanged (gchar *sessId,
    const gchar *streamId,
    guint componentId, guint state)
{
  try {
    IceComponentState::type type;
    std::shared_ptr<IceConnection> connectionState;
    std::map < std::string, std::shared_ptr<IceConnection>>::iterator it;
    std::string key;

    switch (state) {
    case ICE_STATE_DISCONNECTED:
      type = IceComponentState::DISCONNECTED;
      break;

    case ICE_STATE_GATHERING:
      type = IceComponentState::GATHERING;
      break;

    case ICE_STATE_CONNECTING:
      type = IceComponentState::CONNECTING;
      break;

    case ICE_STATE_CONNECTED:
      type = IceComponentState::CONNECTED;
      break;

    case ICE_STATE_READY:
      type = IceComponentState::READY;
      break;

    case ICE_STATE_FAILED:
      type = IceComponentState::FAILED;
      break;

    default:
      type = IceComponentState::FAILED;
      break;
    }

    IceComponentState *componentState_event = new IceComponentState (type);
    IceComponentState *newComponentState_event = new IceComponentState (type);
    IceComponentState *componentState_property = new IceComponentState (type);
    OnIceComponentStateChanged event (shared_from_this(),
                                      OnIceComponentStateChanged::getName(),
                                      atoi (streamId), componentId,
                                      std::shared_ptr<IceComponentState> (componentState_event) );
    IceComponentStateChange newEvent (shared_from_this(),
                                      IceComponentStateChange::getName(),
                                      atoi (streamId), componentId,
                                      std::shared_ptr<IceComponentState> (newComponentState_event) );

    connectionState = std::make_shared< IceConnection> (streamId, componentId,
                      std::shared_ptr<IceComponentState> (componentState_property) );
    key = std::string (streamId) + '_' + std::to_string (componentId);

    std::unique_lock<std::mutex> mutex (mut);
    it = iceConnectionState.find (key);
    iceConnectionState[key] = connectionState;
    iceConnectionState.insert (std::pair
                               <std::string, std::shared_ptr <IceConnection>> (key, connectionState) );

    signalOnIceComponentStateChanged (event);
    signalIceComponentStateChange (newEvent);
  } catch (std::bad_weak_ptr &e) {
  }
}

void WebRtcEndpointImpl::newSelectedPairFull (gchar *sessId,
    const gchar *streamId,
    guint componentId, KmsIceCandidate *localCandidate,
    KmsIceCandidate *remoteCandidate)
{
  std::shared_ptr<IceCandidatePair > candidatePair;
  std::string key;
  std::map<std::string, std::shared_ptr <IceCandidatePair>>::iterator it;

  GST_DEBUG_OBJECT (element,
      "New candidate pair selected, local: '%s', remote: '%s'"
      ", stream_id: '%s', component_id: %d",
      kms_ice_candidate_get_candidate (localCandidate),
      kms_ice_candidate_get_candidate (remoteCandidate),
      streamId, componentId);

  candidatePair = std::make_shared< IceCandidatePair > (streamId,
                  componentId,
                  kms_ice_candidate_get_candidate (localCandidate),
                  kms_ice_candidate_get_candidate (remoteCandidate) );
  key = std::string (streamId) + "_" + std::to_string (componentId);

  it = candidatePairs.find (key);

  if (it != candidatePairs.end() ) {
    candidatePairs.erase (it);
  }

  candidatePairs.insert (std::pair
                         <std::string, std::shared_ptr <IceCandidatePair>> (key, candidatePair) );

  try {
    NewCandidatePairSelected event (shared_from_this(),
                                    NewCandidatePairSelected::getName(), candidatePair);

    signalNewCandidatePairSelected (event);
  } catch (std::bad_weak_ptr &e) {
  }
}

void
WebRtcEndpointImpl::onDataChannelOpened (gchar *sessId, guint stream_id)
{
  try {
    OnDataChannelOpened event (shared_from_this(), OnDataChannelOpened::getName(),
                               stream_id);
    DataChannelOpen newEvent (shared_from_this(), DataChannelOpen::getName(),
                              stream_id);
    signalOnDataChannelOpened (event);
    signalDataChannelOpen (newEvent);
  } catch (std::bad_weak_ptr &e) {
  }
}

void
WebRtcEndpointImpl::onDataChannelClosed (gchar *sessId, guint stream_id)
{
  try {
    OnDataChannelClosed event (shared_from_this(), OnDataChannelClosed::getName(),
                               stream_id);
    DataChannelClose newEvent (shared_from_this(), DataChannelClose::getName(),
                               stream_id);
    signalOnDataChannelClosed (event);
    signalDataChannelClose (newEvent);
  } catch (std::bad_weak_ptr &e) {
  }
}

void WebRtcEndpointImpl::postConstructor ()
{
  BaseRtpEndpointImpl::postConstructor ();

  handlerOnIceCandidate = register_signal_handler (G_OBJECT (element),
                          "on-ice-candidate",
                          std::function <void (GstElement *, gchar *, KmsIceCandidate *) >
                          (std::bind (&WebRtcEndpointImpl::onIceCandidate, this,
                                      std::placeholders::_2, std::placeholders::_3) ),
                          std::dynamic_pointer_cast<WebRtcEndpointImpl>
                          (shared_from_this() ) );

  handlerOnIceGatheringDone = register_signal_handler (G_OBJECT (element),
                              "on-ice-gathering-done",
                              std::function <void (GstElement *, gchar *) >
                              (std::bind (&WebRtcEndpointImpl::onIceGatheringDone, this,
                                          std::placeholders::_2) ),
                              std::dynamic_pointer_cast<WebRtcEndpointImpl>
                              (shared_from_this() ) );

  handlerOnIceComponentStateChanged = register_signal_handler (G_OBJECT (element),
                                      "on-ice-component-state-changed",
                                      std::function <void (GstElement *, gchar *, gchar *, guint, guint) >
                                      (std::bind (&WebRtcEndpointImpl::onIceComponentStateChanged, this,
                                          std::placeholders::_2, std::placeholders::_3, std::placeholders::_4,
                                          std::placeholders::_5) ),
                                      std::dynamic_pointer_cast<WebRtcEndpointImpl>
                                      (shared_from_this() ) );

  handlerNewSelectedPairFull = register_signal_handler (G_OBJECT (element),
                               "new-selected-pair-full",
                               std::function
                               <void (GstElement *, gchar *, gchar *, guint, KmsIceCandidate *, KmsIceCandidate *) >
                               (std::bind (&WebRtcEndpointImpl::newSelectedPairFull, this,
                                   std::placeholders::_2, std::placeholders::_3, std::placeholders::_4,
                                   std::placeholders::_5, std::placeholders::_6) ),
                               std::dynamic_pointer_cast<WebRtcEndpointImpl>
                               (shared_from_this() ) );

  handlerOnDataChannelOpened = register_signal_handler (G_OBJECT (element),
                               "data-channel-opened",
                               std::function <void (GstElement *, gchar *, guint) >
                               (std::bind (&WebRtcEndpointImpl::onDataChannelOpened, this,
                                   std::placeholders::_2, std::placeholders::_3) ),
                               std::dynamic_pointer_cast<WebRtcEndpointImpl>
                               (shared_from_this() ) );

  handlerOnDataChannelClosed = register_signal_handler (G_OBJECT (element),
                               "data-channel-closed",
                               std::function <void (GstElement *, gchar *, guint) >
                               (std::bind (&WebRtcEndpointImpl::onDataChannelClosed, this,
                                   std::placeholders::_2, std::placeholders::_3) ),
                               std::dynamic_pointer_cast<WebRtcEndpointImpl>
                               (shared_from_this() ) );
}

std::string
WebRtcEndpointImpl::getCerficateFromFile (std::string &path)
{
  std::ifstream inFile;
  std::stringstream strStream;
  std::string certificate;

  //check if the uri is absolute or relative
  checkUri (path);
  GST_INFO ("pemCertificate in: %s\n", path.c_str() );

  inFile.open (path);
  strStream << inFile.rdbuf();
  certificate = strStream.str();

  if (!CertificateManager::isCertificateValid (certificate) ) {
    GST_ERROR ("Certificate in file %s is not valid", path.c_str () );
    return "";
  }

  return certificate;
}

WebRtcEndpointImpl::WebRtcEndpointImpl (const boost::property_tree::ptree &conf,
                                        std::shared_ptr<MediaPipeline>
                                        mediaPipeline, bool useDataChannels,
                                        std::shared_ptr<CertificateKeyType> certificateKeyType) :
  BaseRtpEndpointImpl (conf,
                       std::dynamic_pointer_cast<MediaObjectImpl>
                       (mediaPipeline), FACTORY_NAME)
{
  uint stunPort;
  std::string stunAddress;
  std::string turnURL;

  std::call_once (check_openh264, check_support_for_h264);
  std::call_once (certificates_flag,
                  std::bind (&WebRtcEndpointImpl::generateDefaultCertificates, this) );

  if (useDataChannels) {
    g_object_set (element, "use-data-channels", TRUE, NULL);
  }

  remove_not_supported_codecs (element);

  //set properties
  try {
    stunPort = getConfigValue <uint, WebRtcEndpoint> ("stunServerPort");
  } catch (std::exception &) {
    GST_INFO ("STUN server Port not found in config;"
              " using default value: %d", DEFAULT_STUN_PORT);
    stunPort = DEFAULT_STUN_PORT;
  }

  if (stunPort != 0) {
    try {
      stunAddress = getConfigValue
                    <std::string, WebRtcEndpoint> ("stunServerAddress");
    } catch (boost::property_tree::ptree_error &) {
      GST_INFO ("STUN server IP address not found in config;"
                " NAT traversal requires either STUN or TURN server");
    }

    if (!stunAddress.empty()) {
      GST_INFO ("Using STUN reflexive server IP: %s", stunAddress.c_str());
      GST_INFO ("Using STUN reflexive server Port: %d", stunPort);

      g_object_set (G_OBJECT (element), "stun-server-port", stunPort, NULL);
      g_object_set (G_OBJECT (element), "stun-server", stunAddress.c_str(), NULL);
    }
  }

  try {
    turnURL = getConfigValue <std::string, WebRtcEndpoint> ("turnURL");
    GST_INFO ("Using TURN relay server: %s", turnURL.c_str());
    g_object_set (G_OBJECT (element), "turn-url", turnURL.c_str(), NULL);
  } catch (boost::property_tree::ptree_error &) {
    GST_INFO ("TURN server IP address not found in config;"
              " NAT traversal requires either STUN or TURN server");
  }

  switch (certificateKeyType->getValue () ) {
  case CertificateKeyType::RSA: {
    if (defaultCertificateRSA != "") {
      g_object_set ( G_OBJECT (element), "pem-certificate",
                     defaultCertificateRSA.c_str(),
                     NULL);
    }

    break;
  }

  case CertificateKeyType::ECDSA: {
    if (defaultCertificateECDSA != "") {
      g_object_set ( G_OBJECT (element), "pem-certificate",
                     defaultCertificateECDSA.c_str(),
                     NULL);
    }

    break;
  }

  default:
    GST_ERROR ("Certificate key not supported");
  }
}

WebRtcEndpointImpl::~WebRtcEndpointImpl()
{
  if (handlerOnIceCandidate > 0) {
    unregister_signal_handler (element, handlerOnIceCandidate);
  }

  if (handlerOnIceGatheringDone > 0) {
    unregister_signal_handler (element, handlerOnIceGatheringDone);
  }

  if (handlerOnIceComponentStateChanged > 0) {
    unregister_signal_handler (element, handlerOnIceComponentStateChanged);
  }

  if (handlerOnDataChannelOpened > 0) {
    unregister_signal_handler (element, handlerOnDataChannelOpened);
  }

  if (handlerOnDataChannelClosed > 0) {
    unregister_signal_handler (element, handlerOnDataChannelClosed);
  }

  if (handlerNewSelectedPairFull > 0) {
    unregister_signal_handler (element, handlerNewSelectedPairFull);
  }
}

std::string
WebRtcEndpointImpl::getStunServerAddress ()
{
  std::string stunServerAddress;
  gchar *ret;

  g_object_get ( G_OBJECT (element), "stun-server", &ret, NULL);

  if (ret != NULL) {
    stunServerAddress = std::string (ret);
    g_free (ret);
  }

  return stunServerAddress;
}

void
WebRtcEndpointImpl::setStunServerAddress (const std::string &stunServerAddress)
{
  g_object_set ( G_OBJECT (element), "stun-server",
                 stunServerAddress.c_str(),
                 NULL);
}

int
WebRtcEndpointImpl::getStunServerPort ()
{
  int ret;

  g_object_get ( G_OBJECT (element), "stun-server-port", &ret, NULL);

  return ret;
}

void
WebRtcEndpointImpl::setStunServerPort (int stunServerPort)
{
  g_object_set ( G_OBJECT (element), "stun-server-port",
                 stunServerPort, NULL);
}

std::string
WebRtcEndpointImpl::getTurnUrl ()
{
  std::string turnUrl;
  gchar *ret;

  g_object_get ( G_OBJECT (element), "turn-url", &ret, NULL);

  if (ret != NULL) {
    turnUrl = std::string (ret);
    g_free (ret);
  }

  return turnUrl;
}

void
WebRtcEndpointImpl::setTurnUrl (const std::string &turnUrl)
{
  g_object_set ( G_OBJECT (element), "turn-url",
                 turnUrl.c_str(),
                 NULL);
}

std::vector<std::shared_ptr<IceCandidatePair>>
    WebRtcEndpointImpl::getICECandidatePairs ()
{
  std::vector<std::shared_ptr<IceCandidatePair>> candidates;
  std::map<std::string, std::shared_ptr <IceCandidatePair>>::iterator it;

  for (it = candidatePairs.begin(); it != candidatePairs.end(); it++) {
    candidates.push_back ( (*it).second);
  }

  return candidates;
}

std::vector<std::shared_ptr<IceConnection>>
    WebRtcEndpointImpl::getIceConnectionState ()
{
  std::vector<std::shared_ptr<IceConnection>> connections;
  std::map<std::string, std::shared_ptr <IceConnection>>::iterator it;
  std::unique_lock<std::mutex> mutex (mut);

  for (it = iceConnectionState.begin(); it != iceConnectionState.end(); it++) {
    connections.push_back ( (*it).second);
  }

  return connections;
}

void
WebRtcEndpointImpl::gatherCandidates ()
{
  gboolean ret;

  g_signal_emit_by_name (element, "gather-candidates", this->sessId.c_str (),
                         &ret);

  if (!ret) {
    throw KurentoException (ICE_GATHER_CANDIDATES_ERROR,
                            "Error gathering candidates");
  }
}

void
WebRtcEndpointImpl::addIceCandidate (std::shared_ptr<IceCandidate> candidate)
{
  gboolean ret = FALSE;
  std::string cand_str = candidate->getCandidate();
  std::string mid_str = candidate->getSdpMid ();
  guint8 sdp_m_line_index = candidate->getSdpMLineIndex ();
  KmsIceCandidate *cand = kms_ice_candidate_new (cand_str.c_str(),
                          mid_str.c_str(),
                          sdp_m_line_index, NULL);

  if (cand) {
    g_signal_emit_by_name (element, "add-ice-candidate", this->sessId.c_str (),
                           cand, &ret);
    g_object_unref (cand);
  }

  if (!ret) {
    throw KurentoException (ICE_ADD_CANDIDATE_ERROR, "Error adding candidate");
  }
}

void
WebRtcEndpointImpl::createDataChannel ()
{
  createDataChannel ("", true, -1, -1, "");
}

void
WebRtcEndpointImpl::createDataChannel (const std::string &label)
{
  createDataChannel (label, true, -1, -1, "");
}

void
WebRtcEndpointImpl::createDataChannel (const std::string &label, bool ordered)
{
  createDataChannel (label, ordered, -1, -1, "");
}

void
WebRtcEndpointImpl::createDataChannel (const std::string &label, bool ordered,
                                       int maxPacketLifeTime)
{
  createDataChannel (label, ordered, maxPacketLifeTime, -1, "");
}

void
WebRtcEndpointImpl::createDataChannel (const std::string &label, bool ordered,
                                       int maxPacketLifeTime, int maxRetransmits)
{
  createDataChannel (label, ordered, maxPacketLifeTime, maxRetransmits, "");
}

void
WebRtcEndpointImpl::createDataChannel (const std::string &label, bool ordered,
                                       int maxPacketLifeTime, int maxRetransmits, const std::string &protocol)
{
  gint lifeTime, retransmits, stream_id;
  gboolean supported;

  g_signal_emit_by_name (element, "get-data-channel-supported",
                         this->sessId.c_str (), &supported);

  if (!supported) {
    throw KurentoException (MEDIA_OBJECT_OPERATION_NOT_SUPPORTED,
                            "Data channels are not supported");
  }

  /* Less than one values mean that parameters are disabled */
  if (maxPacketLifeTime < 0) {
    maxPacketLifeTime = -1;
  }

  if (maxRetransmits < 0) {
    maxRetransmits = -1;
  }

  if (maxPacketLifeTime != -1 && maxRetransmits != -1) {
    /* Both values are incompatible.                                     */
    /* http://w3c.github.io/webrtc-pc/#dom-datachannel-maxpacketlifetime */
    throw KurentoException (MEDIA_OBJECT_ILLEGAL_PARAM_ERROR, "Syntax error");
  }

  if (maxPacketLifeTime > G_MAXUSHORT) {
    GST_WARNING ("maxPacketLifeTime can not be bigger than %u. Setting it to that value",
                 G_MAXUSHORT);
    lifeTime = G_MAXUSHORT;
  } else {
    lifeTime = maxPacketLifeTime;
  }

  if (maxRetransmits > G_MAXUSHORT) {
    GST_WARNING ("maxRetransmits can not be bigger than %u. Setting it to that value",
                 G_MAXUSHORT);
    retransmits = G_MAXUSHORT;
  } else {
    retransmits = maxRetransmits;
  }

  /* Create the data channel */
  g_signal_emit_by_name (element, "create-data-channel", this->sessId.c_str (),
                         ordered, lifeTime,
                         retransmits, label.c_str(), protocol.c_str(),
                         &stream_id);

  if (stream_id < 0) {
    throw KurentoException (UNEXPECTED_ERROR, "Can not create data channel");
  }

  GST_DEBUG ("Creating data channel with stream id %d", stream_id);
}

void
WebRtcEndpointImpl::closeDataChannel (int channelId)
{
  gboolean supported;

  g_signal_emit_by_name (element, "get-data-channel-supported",
                         this->sessId.c_str (), &supported);

  if (!supported) {
    throw KurentoException (MEDIA_OBJECT_OPERATION_NOT_SUPPORTED,
                            "Data channels are not supported");
  }

  /* Destroy the data channel */
  g_signal_emit_by_name (element, "destroy-data-channel", this->sessId.c_str (),
                         channelId);
}

static std::shared_ptr<RTCDataChannelState>
getRTCDataChannelState (KmsWebRtcDataChannelState state)
{
  std::shared_ptr<RTCDataChannelState> rtcDataChannelState;

  switch (state) {
  case KMS_WEB_RTC_DATA_CHANNEL_STATE_CONNECTING:
    rtcDataChannelState =  std::make_shared <RTCDataChannelState>
                           (RTCDataChannelState::connecting);
    break;

  case KMS_WEB_RTC_DATA_CHANNEL_STATE_OPEN:
    rtcDataChannelState = std::make_shared <RTCDataChannelState>
                          (RTCDataChannelState::open);
    break;

  case KMS_WEB_RTC_DATA_CHANNEL_STATE_CLOSING:
    rtcDataChannelState = std::make_shared <RTCDataChannelState>
                          (RTCDataChannelState::closing);
    break;

  case KMS_WEB_RTC_DATA_CHANNEL_STATE_CLOSED:
    rtcDataChannelState = std::make_shared <RTCDataChannelState>
                          (RTCDataChannelState::closed);
    break;
  }

  return rtcDataChannelState;
}

static std::shared_ptr<RTCDataChannelStats>
createtRTCDataChannelStats (const GstStructure *stats)
{
  KmsWebRtcDataChannelState state;
  guint64 messages_sent, message_recv, bytes_sent, bytes_recv;
  gchar *id, *label, *protocol;
  guint channelid;

  gst_structure_get (stats, "channel-id", G_TYPE_UINT, &channelid, "label",
                     G_TYPE_STRING, &label, "protocol", G_TYPE_STRING, &protocol, "id",
                     G_TYPE_STRING, &id, "state", G_TYPE_UINT, &state, "bytes-sent",
                     G_TYPE_UINT64, &bytes_sent, "bytes-recv", G_TYPE_UINT64,
                     &bytes_recv, "messages-sent", G_TYPE_UINT64, &messages_sent,
                     "messages-recv", G_TYPE_UINT64, &message_recv, NULL);

  std::shared_ptr<RTCDataChannelStats> rtcDataStats =
    std::make_shared <RTCDataChannelStats> (id,
        std::make_shared <StatsType> (StatsType::datachannel), 0.0, label,
        protocol, channelid, getRTCDataChannelState (state), messages_sent,
        bytes_sent, message_recv, bytes_recv);

  g_free (protocol);
  g_free (label);
  g_free (id);

  return rtcDataStats;
}

static std::shared_ptr<RTCPeerConnectionStats>
createtRTCPeerConnectionStats (const GstStructure *stats)
{
  guint opened, closed;
  gchar *id;

  gst_structure_get (stats, "data-channels-opened", G_TYPE_UINT, &opened,
                     "data-channels-closed", G_TYPE_UINT, &closed,
                     "id", G_TYPE_STRING, &id, NULL);

  std::shared_ptr<RTCPeerConnectionStats> peerConnStats =
    std::make_shared <RTCPeerConnectionStats> (id,
        std::make_shared <StatsType> (StatsType::session), 0.0, opened, closed);
  g_free (id);

  return peerConnStats;
}

static void
collectRTCDataChannelStats (std::map <std::string, std::shared_ptr<Stats>>
                            &statsReport, double timestamp, const GstStructure *stats)
{
  gint i, n;

  n = gst_structure_n_fields (stats);

  for (i = 0; i < n; i++) {
    std::shared_ptr<RTCDataChannelStats> rtcDataStats;
    const GValue *value;
    const gchar *name;

    name = gst_structure_nth_field_name (stats, i);

    if (!g_str_has_prefix (name, "data-channel-") ) {
      continue;
    }

    value = gst_structure_get_value (stats, name);

    if (!GST_VALUE_HOLDS_STRUCTURE (value) ) {
      gchar *str_val;

      str_val = g_strdup_value_contents (value);
      GST_WARNING ("Unexpected field type (%s) = %s", name, str_val);
      g_free (str_val);

      continue;
    }

    rtcDataStats = createtRTCDataChannelStats (gst_value_get_structure (value) );
    rtcDataStats->setTimestamp (timestamp);
    statsReport[rtcDataStats->getId ()] = rtcDataStats;
  }

  std::shared_ptr<RTCPeerConnectionStats> peerConnStats =
    createtRTCPeerConnectionStats (stats);
  peerConnStats->setTimestamp (timestamp);
  statsReport[peerConnStats->getId ()] = peerConnStats;
}

void
WebRtcEndpointImpl::fillStatsReport (std::map
                                     <std::string, std::shared_ptr<Stats>>
                                     &report, const GstStructure *stats, double timestamp)
{
  const GstStructure *data_stats = NULL;

  BaseRtpEndpointImpl::fillStatsReport (report, stats, timestamp);

  data_stats = kms_utils_get_structure_by_name (stats,
               KMS_DATA_SESSION_STATISTICS_FIELD);

  if (data_stats != NULL) {
    return collectRTCDataChannelStats (report, timestamp, data_stats);
  }
}

MediaObjectImpl *
WebRtcEndpointImplFactory::createObject (const boost::property_tree::ptree
    &conf, std::shared_ptr<MediaPipeline>
    mediaPipeline, bool useDataChannels,
    std::shared_ptr<CertificateKeyType> certificateKeyType) const
{
  return new WebRtcEndpointImpl (conf, mediaPipeline, useDataChannels,
                                 certificateKeyType);
}

WebRtcEndpointImpl::StaticConstructor WebRtcEndpointImpl::staticConstructor;

WebRtcEndpointImpl::StaticConstructor::StaticConstructor()
{
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, GST_DEFAULT_NAME, 0,
                           GST_DEFAULT_NAME);
}

} /* kurento */
