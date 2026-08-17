import esphome.codegen as cg

from tests.testing_helpers import ComponentManifestOverride


def override_manifest(manifest: ComponentManifestOverride) -> None:
    # api_frame_helper.h's body is entirely guarded by USE_API; without this
    # override (normally emitted by the api component's own to_code, which is
    # suppressed here) that guarded content -- including the compile-time
    # reentrancy test right below may_write_now() and APIOverflowBuffer's own
    # compile-time tests -- would never be compiled in the unit-test build,
    # silently proving nothing.
    async def to_code_testing(config):
        cg.add_define("USE_API")
        cg.add_define("USE_API_PLAINTEXT")
        cg.add_define("MAX_API_CONNECTIONS", 8)
        cg.add_define("API_MAX_SEND_QUEUE", 8)
        # The socket dependency's to_code is suppressed as well, so pick its
        # host implementation here (BSD sockets on the host test platform).
        cg.add_define("USE_SOCKET_IMPL_BSD_SOCKETS")
        # api_server.cpp registers itself as a controller unconditionally;
        # the registry is normally enabled by core config codegen.
        cg.add_define("USE_CONTROLLER_REGISTRY")
        cg.add_define("CONTROLLER_REGISTRY_MAX", 1)
        # Compile-time test dependency; normally added by the api component's
        # own to_code, which is suppressed here.
        cg.add_library(
            "compile-time-unit-testing",
            None,
            "https://github.com/sfalasca/compile-time-unit-testing#v1.0.1",
        )

    manifest.to_code = to_code_testing
