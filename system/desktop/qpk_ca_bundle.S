/* SPDX-License-Identifier: Apache-2.0 */

  .section .rodata.qpk_ca_bundle, "a", @progbits
  .balign 4
  .global g_qpk_ca_bundle
  .global g_qpk_ca_bundle_end

g_qpk_ca_bundle:
  .incbin "qpk_cacert.pem"
  .byte 0
g_qpk_ca_bundle_end:

