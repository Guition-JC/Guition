# FactoryDemo iPerf component override

This project component is the checked-in FactoryDemo copy of the resolved
`espressif/iperf` 1.0.3 source at upstream commit
`f9e541b232a034bfa676eddf3c6bba7d262b3a66`. It keeps the public instance API
and classic iPerf core behavior while adding the FactoryDemo-required TCP
source bind, bounded TCP connect cancellation, and bounded task/socket
teardown safeguards.

The application still declares `espressif/iperf` in `main/idf_component.yml`,
and `dependencies.lock` pins the managed reference used for comparison. At
build discovery time, however, the physical `components/iperf` directory has
higher priority than `managed_components/espressif__iperf` and shadows it.
The generated build graph is the source of truth: `build/build.ninja` compiles
`components/iperf/iperf.c` into `build/esp-idf/iperf/libiperf.a`.

When upgrading the dependency, first update the managed/pinned source, compare
it with this directory, then re-apply and re-review the local safety changes.
