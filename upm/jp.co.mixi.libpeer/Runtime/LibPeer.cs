using System;
using System.Runtime.InteropServices;

namespace Mixi.LibPeer
{
    /// <summary>
    /// Native library loader for libpeer
    /// </summary>
    internal static class LibPeerNative
    {
#if UNITY_IOS && !UNITY_EDITOR
        private const string LIB = "__Internal";
#elif UNITY_ANDROID && !UNITY_EDITOR
        private const string LIB = "peer";
#elif UNITY_STANDALONE_OSX || UNITY_EDITOR_OSX
        private const string LIB = "peer";
#elif UNITY_STANDALONE_WIN || UNITY_EDITOR_WIN
        private const string LIB = "peer";
#elif UNITY_STANDALONE_LINUX || UNITY_EDITOR_LINUX
        private const string LIB = "peer";
#else
        private const string LIB = "peer";
#endif

        // ============================================================
        // peer.h
        // ============================================================

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int peer_init();

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void peer_deinit();

        // ============================================================
        // peer_connection.h
        // ============================================================

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr peer_connection_state_to_string(PeerConnectionState state);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern PeerConnectionState peer_connection_get_state(IntPtr pc);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr peer_connection_create(ref PeerConfigurationNative config);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void peer_connection_destroy(IntPtr pc);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void peer_connection_close(IntPtr pc);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int peer_connection_loop(IntPtr pc);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int peer_connection_create_datachannel(
            IntPtr pc,
            DecpChannelType channelType,
            ushort priority,
            uint reliabilityParameter,
            [MarshalAs(UnmanagedType.LPStr)] string label,
            [MarshalAs(UnmanagedType.LPStr)] string protocol);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int peer_connection_datachannel_send(
            IntPtr pc,
            IntPtr message,
            UIntPtr len);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int peer_connection_datachannel_send_sid(
            IntPtr pc,
            IntPtr message,
            UIntPtr len,
            ushort sid);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void peer_connection_set_remote_description(
            IntPtr pc,
            [MarshalAs(UnmanagedType.LPStr)] string sdp,
            SdpType sdpType);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void peer_connection_set_local_description(
            IntPtr pc,
            [MarshalAs(UnmanagedType.LPStr)] string sdp,
            SdpType sdpType);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr peer_connection_create_offer(IntPtr pc);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr peer_connection_create_answer(IntPtr pc);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void peer_connection_onicecandidate(
            IntPtr pc,
            OnIceCandidateCallback callback);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void peer_connection_oniceconnectionstatechange(
            IntPtr pc,
            OnIceConnectionStateChangeCallback callback);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void peer_connection_ondatachannel(
            IntPtr pc,
            OnDataChannelMessageCallback onMessage,
            OnDataChannelOpenCallback onOpen,
            OnDataChannelCloseCallback onClose);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int peer_connection_add_ice_candidate(
            IntPtr pc,
            [MarshalAs(UnmanagedType.LPStr)] string iceCandidate);

        [DllImport(LIB, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int peer_connection_lookup_sid(
            IntPtr pc,
            [MarshalAs(UnmanagedType.LPStr)] string label,
            out ushort sid);

        // ============================================================
        // Callbacks (must match C function signatures)
        // ============================================================

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void OnIceCandidateCallback(IntPtr sdpText, IntPtr userData);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void OnIceConnectionStateChangeCallback(PeerConnectionState state, IntPtr userData);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void OnDataChannelMessageCallback(IntPtr msg, UIntPtr len, IntPtr userData, ushort sid);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void OnDataChannelOpenCallback(IntPtr userData);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void OnDataChannelCloseCallback(IntPtr userData);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void OnAudioTrackCallback(IntPtr data, UIntPtr size, IntPtr userData);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void OnVideoTrackCallback(IntPtr data, UIntPtr size, IntPtr userData);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void OnRequestKeyframeCallback(IntPtr userData);
    }

    // ============================================================
    // Enums
    // ============================================================

    public enum SdpType
    {
        Offer = 0,
        Answer = 1
    }

    public enum PeerConnectionState
    {
        Closed = 0,
        New = 1,
        Checking = 2,
        Connected = 3,
        Completed = 4,
        Failed = 5,
        Disconnected = 6
    }

    public enum DataChannelType
    {
        None = 0,
        String = 1,
        Binary = 2
    }

    public enum DecpChannelType
    {
        Reliable = 0x00,
        ReliableUnordered = 0x80,
        PartialReliableRexmit = 0x01,
        PartialReliableRexmitUnordered = 0x81,
        PartialReliableTimed = 0x02,
        PartialReliableTimedUnordered = 0x82
    }

    public enum MediaCodec
    {
        None = 0,
        H264 = 1,
        VP8 = 2,
        MJPEG = 3,
        Opus = 4,
        PCMA = 5,
        PCMU = 6
    }

    // ============================================================
    // Native Structures
    // ============================================================

    [StructLayout(LayoutKind.Sequential)]
    internal struct IceServerNative
    {
        public IntPtr urls;
        public IntPtr username;
        public IntPtr credential;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct PeerConfigurationNative
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 5)]
        public IceServerNative[] iceServers;

        public MediaCodec audioCodec;
        public MediaCodec videoCodec;
        public DataChannelType dataChannel;

        public LibPeerNative.OnAudioTrackCallback onAudioTrack;
        public LibPeerNative.OnVideoTrackCallback onVideoTrack;
        public LibPeerNative.OnRequestKeyframeCallback onRequestKeyframe;
        public IntPtr userData;
    }

    // ============================================================
    // Public Configuration Classes
    // ============================================================

    public class IceServer
    {
        public string Urls { get; set; }
        public string Username { get; set; }
        public string Credential { get; set; }
    }

    public class PeerConfiguration
    {
        public IceServer[] IceServers { get; set; } = new IceServer[5];
        public MediaCodec AudioCodec { get; set; } = MediaCodec.None;
        public MediaCodec VideoCodec { get; set; } = MediaCodec.None;
        public DataChannelType DataChannel { get; set; } = DataChannelType.None;
    }
}
