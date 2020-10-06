﻿// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#nullable enable

using System.Collections.Generic;

namespace BuildXL.Cache.ContentStore.Grpc
{
    /// <summary>
    /// Environmental options for gRPC. Once set, these can't be changed for the entire program lifetime.
    ///
    /// This is based on the GrpcEnvironment as provided by the Grpc Core C# library, and the environment variables
    /// that gRPC Core supports. See here: https://github.com/grpc/grpc/blob/master/doc/environment_variables.md
    /// </summary>
    public class GrpcEnvironmentOptions
    {
        /// <nodoc />
        public enum GrpcVerbosity
        {
            /// <nodoc />
            Disabled,

            /// <nodoc />
            Debug,

            /// <nodoc />
            Info,

            /// <nodoc />
            Error
        }

        /// <summary>
        /// <see cref="global::Grpc.Core.GrpcEnvironment.SetThreadPoolSize(int)"/>
        /// </summary>
        /// <remarks>
        /// Default is set due to this being the legacy default. Does not mean it's actually safe to use.
        /// </remarks>
        public int? ThreadPoolSize { get; set; } = 70;

        /// <summary>
        /// <see cref="global::Grpc.Core.GrpcEnvironment.SetCompletionQueueCount(int)"/>
        /// </summary>
        /// <remarks>
        /// Default is set due to this being the legacy default. Does not mean it's actually safe to use.
        /// </remarks>
        public int? CompletionQueueCount { get; set; } = 70;

        /// <summary>
        /// <see cref="global::Grpc.Core.GrpcEnvironment.SetHandlerInlining(bool)"/>
        /// </summary>
        /// <remarks>
        /// Default is set due to this being the legacy default. Does not mean it's actually safe to use.
        /// </remarks>
        public bool? HandlerInlining { get; set; } = true;

        /// <summary>
        /// gRPC Logging Verbosity
        /// </summary>
        public GrpcVerbosity LoggingVerbosity { get; set; } = GrpcVerbosity.Disabled;

        /// <summary>
        /// Which gRPC tracing logs to enable. For a complete list of available tracing options, see:
        /// https://github.com/grpc/grpc/blob/master/doc/environment_variables.md
        /// </summary>
        /// <remarks>
        /// Setting this without setting <see cref="LoggingVerbosity"/> is useless.
        /// </remarks>
        public List<string>? Trace { get; set; }

        /// <summary>
        /// From gRPC documentation:
        /// 
        /// Declares the interval between two backup polls on client channels. These polls are run in the timer thread
        /// so that gRPC can process connection failures while there is no active polling thread. They help reconnect
        /// disconnected client channels (mostly due to idleness), so that the next RPC on this channel won't fail. Set
        /// to 0 to turn off the backup polls.
        /// </summary>
        public int? ClientChannelBackupPollIntervalMs { get; set; }

        /// <summary>
        /// From gRPC documentation:
        /// 
        /// If set, flow control will be effectively disabled. Max out all values and assume the remote peer does the
        /// same. Thus we can ignore any flow control bookkeeping, error checking, and decision making.
        /// </summary>
        /// <remarks>
        /// gRPC has its own flow control mechanism implemented on top of HTTP2. This setting disables that.
        /// </remarks>
        public bool? ExperimentalDisableFlowControl { get; set; }
    }
}
