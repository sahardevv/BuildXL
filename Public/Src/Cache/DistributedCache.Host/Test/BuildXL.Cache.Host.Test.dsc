// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

namespace Test {
    @@public
    export const dll = BuildXLSdk.cacheTest({
        assemblyName: "BuildXL.Cache.Host.Test",
        sources: globR(d`.`,"*.cs"),
        skipTestRun: BuildXLSdk.restrictTestRunToSomeQualifiers,
        references: [
            ...addIfLazy(BuildXLSdk.isFullFramework, () => [
                NetFx.System.Runtime.Serialization.dll,
                NetFx.System.Xml.dll,
                NetFx.System.Xml.Linq.dll,
            ]),
            ...importFrom("BuildXL.Cache.ContentStore").getSerializationPackages(true),
            Configuration.dll,
            Service.dll,
            importFrom("BuildXL.Cache.ContentStore").Interfaces.dll,
            importFrom("BuildXL.Cache.ContentStore").Distributed.dll,
            importFrom("BuildXL.Cache.ContentStore").Interfaces.dll,
            importFrom("BuildXL.Cache.ContentStore").Library.dll,
            importFrom("BuildXL.Cache.ContentStore").Test.dll,
            importFrom("BuildXL.Cache.ContentStore").InterfacesTest.dll,
            importFrom("BuildXL.Cache.ContentStore").Grpc.dll,
            ...BuildXLSdk.fluentAssertionsWorkaround,

            // Used by Launcher integration test
            importFrom("BuildXL.Utilities").dll,
            importFrom("BuildXL.Cache.ContentStore").App.exe,
            ...addIf(!BuildXLSdk.isFullFramework,
                LauncherServer.withQualifier({targetFramework: "netcoreapp3.1"}).exe
            )
        ],
        tools: {
            csc: {
                noWarnings: [
                    8002, // References ContentStoreApp.exe which is not signed because it uses CLAP
                ]
            },

        },
    });
}
