/* Issue #71: QcWifiWebTransport.hpp's HTTP_SERVICE_DEFINE(qcWebService, ...)
 * needs a matching per-service iterable-section linker fragment - Zephyr's
 * HTTP_RESOURCE_DEFINE()/HTTP_SERVICE_DEFINE() macros don't generate this
 * themselves (confirmed against every in-tree sample that uses them, e.g.
 * samples/net/sockets/http_server's own sections-rom.ld, same pattern).
 * Without it, the qcWebService_list_start/_list_end symbols those macros
 * extern-declare are never defined, so linking the app fails outright
 * rather than producing broken behavior - this isn't optional wiring.
 */
#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(http_resource_desc_qcWebService, Z_LINK_ITERABLE_SUBALIGN)
