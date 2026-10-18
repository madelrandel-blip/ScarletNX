/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Local wireless (ldn) over the LAN, speaking the protocol of the ldn_mitm sysmodule (spacemeowx2/ldn_mitm, MIT),
   which Ryujinx's LdnMitm client speaks too, so a ScarletNX finds the networks of another ScarletNX, of a Ryujinx or
   of a console running ldn_mitm on the same network. UDP port 11452 carries the scan (a Scan broadcast on the
   interface's broadcast address, answered with the host's NetworkInfo), TCP 11452 on the host carries the session (a
   station sends its NodeInfo, the host sends the whole NetworkInfo to every station whenever it changes; closing the
   connection is leaving). The game's own traffic goes straight to the nodes' addresses through its sockets.

   One SnxLdn per emulated console. Its thread receives; the calls below are made from the guest's threads and never
   wait with the lock held. The structures are the guest's (nn::ldn), little-endian, laid out byte for byte. */
#ifndef SNX_NET_LDN_H
#define SNX_NET_LDN_H
#include "common/network/net.h"

enum {
    SNX_LDN_PORT = 11452,
    SNX_LDN_NODES = 8,
    SNX_LDN_STATIONS = 7,
    SNX_LDN_USER_NAME = 33, /* UserNameBytesMax + 1 */
    SNX_LDN_ADVERTISE_MAX = 384,
    SNX_LDN_SSID = 33,
    SNX_LDN_FRAME_MAX = 2048, /* a frame with its header, as ldn_mitm bounds it */
    SNX_LDN_HEADER = 12
};
/* a node's state change since the last read (nn::ldn::NodeLatestUpdate's bits) */
enum { SNX_LDN_NODE_CONNECT = 1, SNX_LDN_NODE_DISCONNECT = 2 };
/* why the station is no longer connected (nn::ldn::DisconnectReason) */
enum { SNX_LDN_REASON_NONE = 0, SNX_LDN_REASON_DISCONNECTED_BY_USER = 1, SNX_LDN_REASON_DESTROYED_BY_USER = 3 };

/* nn::ldn::NodeInfo. ipv4 is in host order, as the guest keeps it (ipv4[0] is the last octet) */
typedef struct SnxLdnNodeInfo {
    uint8_t ipv4[4];
    uint8_t mac[6];
    int8_t node_id;
    uint8_t is_connected;
    uint8_t user_name[SNX_LDN_USER_NAME];
    uint8_t reserved1;
    int16_t version; /* local communication version */
    uint8_t reserved2[16];
} SnxLdnNodeInfo;
typedef char snx_ldn_node_info_is_64_bytes_[sizeof(SnxLdnNodeInfo) == 0x40 ? 1 : -1];

/* nn::ldn::NetworkInfo: the network id, the common part and the ldn part */
typedef struct SnxLdnNetworkInfo {
    uint64_t local_communication_id; /* IntentId */
    uint8_t intent_reserved1[2];
    uint16_t scene_id;
    uint8_t intent_reserved2[4];
    uint64_t session_id[2]; /* SessionId: high, low */
    uint8_t bssid[6];       /* CommonNetworkInfo */
    uint8_t ssid_length;
    uint8_t ssid[SNX_LDN_SSID];
    int16_t channel;
    int8_t link_level;
    uint8_t network_type;
    uint8_t common_reserved[4];
    uint8_t security_parameter[16]; /* LdnNetworkInfo */
    uint16_t security_mode;
    uint8_t accept_policy;
    uint8_t has_action_frame;
    uint8_t ldn_reserved1[2];
    uint8_t node_count_max;
    uint8_t node_count;
    SnxLdnNodeInfo nodes[SNX_LDN_NODES];
    uint8_t ldn_reserved2[2];
    uint16_t advertise_data_size;
    uint8_t advertise_data[SNX_LDN_ADVERTISE_MAX];
    uint8_t ldn_reserved3[0x8c];
    uint64_t random_authentication_id;
} SnxLdnNetworkInfo;
typedef char snx_ldn_network_info_is_1152_bytes_[sizeof(SnxLdnNetworkInfo) == 0x480 ? 1 : -1];

/* nn::ldn::ScanFilter and its flags */
enum {
    SNX_LDN_FILTER_LOCAL_COMMUNICATION_ID = 1,
    SNX_LDN_FILTER_SESSION_ID = 2,
    SNX_LDN_FILTER_NETWORK_TYPE = 4,
    SNX_LDN_FILTER_SSID = 0x10,
    SNX_LDN_FILTER_SCENE_ID = 0x20
};
typedef struct SnxLdnScanFilter {
    uint64_t local_communication_id;
    uint8_t intent_reserved1[2];
    uint16_t scene_id;
    uint8_t intent_reserved2[4];
    uint64_t session_id[2];
    uint32_t network_type;
    uint8_t mac[6];
    uint8_t ssid_length;
    uint8_t ssid[SNX_LDN_SSID];
    uint8_t reserved[0x10];
    uint32_t flag;
} SnxLdnScanFilter;
typedef char snx_ldn_scan_filter_is_96_bytes_[sizeof(SnxLdnScanFilter) == 0x60 ? 1 : -1];

typedef struct SnxLdn SnxLdn;

/* Opens the scan sockets on the interface whose address and mask these are, and starts the thread. changed(arg) is
   called from that thread, without any lock held, whenever the network or its nodes change (a station came or went,
   the host went away). 0, or -SNX_E* (SNX_EADDRINUSE: another console already uses this address on this host, which
   the session port, held from here to close, tells) */
int snx_ldn_open(SnxLdn** out, const uint8_t ip[4], const uint8_t mask[4], void (*changed)(void* arg), void* arg);
/* Leaves any network, stops the thread and closes everything */
void snx_ldn_close(SnxLdn* l);
void snx_ldn_address(const SnxLdn* l, uint8_t ip[4], uint8_t mask[4]);
/* The fake MAC ldn_mitm derives from an address, 02 00 then the four octets */
void snx_ldn_fake_mac(const uint8_t ip[4], uint8_t mac[6]);
/* Fills node as this console: its address (host order), fake MAC, name, version, connected */
void snx_ldn_node_init(const SnxLdn* l, SnxLdnNodeInfo* node, const uint8_t user_name[SNX_LDN_USER_NAME],
                       int16_t version);

/* The host's side. create_network keeps info (node 0 and the rest as the caller built them: snx_ldn_node_init for
   node 0, node_count 1) and takes stations in; -SNX_EBUSY while this console hosts or is in a network.
   set_advertise_data changes the advertisement (-SNX_EINVAL over SNX_LDN_ADVERTISE_MAX) and resends the network to
   the stations; it may be set before the network exists, and stays for the next one */
int snx_ldn_create_network(SnxLdn* l, const SnxLdnNetworkInfo* info);
void snx_ldn_destroy_network(SnxLdn* l);
int snx_ldn_set_advertise_data(SnxLdn* l, const void* data, size_t size);

/* The scanner's side: sends the Scan, gathers the replies for up to wait_ms (it returns 100 ms after the first),
   keeps those filter accepts, and returns how many went to out (at most max). filter may be NULL */
int snx_ldn_scan(SnxLdn* l, const SnxLdnScanFilter* filter, SnxLdnNetworkInfo* out, int max, int wait_ms);

/* The station's side: joins info's network (its node 0's address) as user_name and version, and waits up to wait_ms
   for the host's first SyncNetwork. 0 when connected, -SNX_ETIMEDOUT when the host accepted and never synced,
   -SNX_ECONNREFUSED and the like when it couldn't be reached, -SNX_EINVAL for a network with no nodes */
int snx_ldn_connect(SnxLdn* l, const SnxLdnNetworkInfo* info, const uint8_t user_name[SNX_LDN_USER_NAME],
                    int16_t version, int wait_ms);
/* leaves the host's network (the station's own choice: the reason stays DisconnectedByUser) */
void snx_ldn_disconnect(SnxLdn* l);

/* 1 and the current network when this console is its host or a connected station, else 0. updates, when not NULL,
   gets each node's SNX_LDN_NODE_* bits since the last call that read them, which this call clears */
int snx_ldn_network_info(SnxLdn* l, SnxLdnNetworkInfo* out, uint8_t updates[SNX_LDN_NODES]);
/* 1 while this console hosts a network or is connected to one */
int snx_ldn_connected(SnxLdn* l);
/* why a station is no longer connected: SNX_LDN_REASON_* */
int snx_ldn_disconnect_reason(SnxLdn* l);

/* The wire frames, for the tests: a frame is the 12 byte header (magic, type, compressed, length, decompressed
   length, two reserved bytes) and its payload, run-length encoded on its zeros when that is shorter. frame returns
   the frame's size, or 0 when it doesn't fit max. unframe takes the first frame of in: 1 with its type, payload,
   payload size and the bytes it used, 0 for an incomplete one, -1 for what isn't a frame (the magic, a payload over
   the limit, a bad run-length stream) */
enum { SNX_LDN_SCAN = 0, SNX_LDN_SCAN_RESPONSE = 1, SNX_LDN_CONNECT = 2, SNX_LDN_SYNC_NETWORK = 3 };
size_t snx_ldn_frame(uint8_t type, const void* payload, size_t size, uint8_t* out, size_t max);
int snx_ldn_unframe(const uint8_t* in, size_t size, uint8_t* type, uint8_t* payload, size_t max, size_t* payload_size,
                    size_t* used);

#endif
