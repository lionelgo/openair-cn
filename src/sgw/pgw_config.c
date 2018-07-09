/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the Apache License, Version 2.0  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file spgw_config.c
  \brief
  \author Lionel Gauthier
  \company Eurecom
  \email: lionel.gauthier@eurecom.fr
*/
#define PGW
#define PGW_CONFIG_C

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <netdb.h>
#include <pthread.h>

#include "bstrlib.h"
#include <libconfig.h>

#include "assertions.h"
#include "dynamic_memory_check.h"
#include "log.h"
#include "intertask_interface.h"
#include "if.h"
#include "async_system.h"
#include "common_defs.h"
#include "pgw_pcef_emulation.h"
#include "spgw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef LIBCONFIG_LONG
#  define libconfig_int long
#else
#  define libconfig_int int
#endif

#define SYSTEM_CMD_MAX_STR_SIZE 512

//------------------------------------------------------------------------------
void pgw_config_init (pgw_config_t * config_pP)
{
  memset ((char *)config_pP, 0, sizeof (*config_pP));
  pthread_rwlock_init (&config_pP->rw_lock, NULL);
  STAILQ_INIT (&config_pP->ipv4_pool_list);
}

//------------------------------------------------------------------------------
int pgw_config_process (pgw_config_t * config_pP)
{
  struct in_addr                          addr_start, addr_mask;
  uint64_t                                counter64 = 0;
  conf_ipv4_list_elm_t                   *ip4_ref = NULL;

#if ENABLE_LIBGTPNL
  async_system_command (TASK_ASYNC_SYSTEM, PGW_ABORT_ON_ERROR, "iptables -t mangle -F OUTPUT");
  async_system_command (TASK_ASYNC_SYSTEM, PGW_ABORT_ON_ERROR, "iptables -t mangle -F POSTROUTING");

  if (config_pP->masquerade_SGI) {
    async_system_command (TASK_ASYNC_SYSTEM, PGW_ABORT_ON_ERROR, "iptables -t nat -F PREROUTING");
  }
#endif

  if (get_mtu_from_iface(config_pP->ipv4.if_name_S5_S8, &config_pP->ipv4.mtu_S5_S8)) {
    OAILOG_CRITICAL(LOG_SPGW_APP, "Failed to probe S5-S8 inet addr\n");
  }

  // GET SGi informations
  if (get_mtu_from_iface(config_pP->ipv4.if_name_S5_S8, &config_pP->ipv4.mtu_SGI)) {
    OAILOG_CRITICAL(LOG_SPGW_APP, "Failed to probe S5-S8 inet addr\n");
  }

  for (int i = 0; i < config_pP->num_ue_pool; i++) {

    memcpy (&addr_start, &config_pP->ue_pool_addr[i], sizeof (struct in_addr));
    memcpy (&addr_mask, &config_pP->ue_pool_addr[i], sizeof (struct in_addr));
    addr_mask.s_addr = addr_mask.s_addr & htonl (0xFFFFFFFF << (32 - config_pP->ue_pool_mask[i]));

    if (memcmp (&addr_start, &addr_mask, sizeof (struct in_addr)) ) {
      AssertFatal (0, "BAD IPV4 ADDR CONFIG/MASK PAIRING %s/%d addr %X mask %X\n",
          inet_ntoa(config_pP->ue_pool_addr[i]), config_pP->ue_pool_mask[i], addr_start.s_addr, addr_mask.s_addr);
    }

    counter64 = 0x00000000FFFFFFFF >> config_pP->ue_pool_mask[i];  // address Prefix_mask/0..0 not valid
    /* .1 reserved for gateway
     * the last address is reserved traditionally (.255 in the case of mask 24)
     */
    counter64 = counter64 - 2;

    //Any math should be applied onto host byte order i.e. ntohl()
    addr_start.s_addr = htonl( ntohl(addr_start.s_addr) + 2 );
    do {
      ip4_ref = calloc (1, sizeof (conf_ipv4_list_elm_t));
      ip4_ref->addr.s_addr = addr_start.s_addr;
      STAILQ_INSERT_TAIL (&config_pP->ipv4_pool_list, ip4_ref, ipv4_entries);

      if (config_pP->arp_ue_linux) {
         async_system_command (TASK_ASYNC_SYSTEM, PGW_ABORT_ON_ERROR, "arp -nDs %s %s pub", inet_ntoa(ip4_ref->addr), bdata(config_pP->ipv4.if_name_SGI));
      }

      counter64 = counter64 - 1;
      addr_start.s_addr = htonl( ntohl(addr_start.s_addr) + 1 );
    } while (counter64 > 0);

    //---------------
#if ENABLE_LIBGTPNL
    if (config_pP->masquerade_SGI) {
      async_system_command (TASK_ASYNC_SYSTEM, PGW_ABORT_ON_ERROR, "iptables -t nat -I POSTROUTING -s %s/%d -o %s  ! --protocol sctp -j SNAT --to-source %s",
          inet_ntoa(config_pP->ue_pool_addr[i]), config_pP->ue_pool_mask[i],
          bdata(config_pP->ipv4.if_name_SGI), str_sgi);
    }
#endif

    uint32_t min_mtu = config_pP->ipv4.mtu_SGI;
    // 36 = GTPv1-U min header size
    if ((config_pP->ipv4.mtu_S5_S8 - 36) < min_mtu) {
      min_mtu = config_pP->ipv4.mtu_S5_S8 - 36;
    }
#if ENABLE_LIBGTPNL
    if (config_pP->ue_tcp_mss_clamp) {
      async_system_command (TASK_ASYNC_SYSTEM, PGW_ABORT_ON_ERROR, "iptables -t mangle -I FORWARD -s %s/%d   -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss %u",
          inet_ntoa(config_pP->ue_pool_addr[i]), config_pP->ue_pool_mask[i], min_mtu - 40);

      async_system_command (TASK_ASYNC_SYSTEM, PGW_ABORT_ON_ERROR, "iptables -t mangle -I FORWARD -d %s/%d -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss %u",
          inet_ntoa(config_pP->ue_pool_addr[i]), config_pP->ue_pool_mask[i], min_mtu - 40);
    }
#endif

    if (config_pP->pcef.enabled) {
#if ENABLE_LIBGTPNL
      if (config_pP->pcef.tcp_ecn_enabled) {
        async_system_command (TASK_ASYNC_SYSTEM, PGW_ABORT_ON_ERROR, "sysctl -w net.ipv4.tcp_ecn=1");
      }
#endif
      pgw_pcef_emulation_init(config_pP);
    }
  }
  return 0;
}

//------------------------------------------------------------------------------
int pgw_config_parse_file (pgw_config_t * config_pP)
{
  config_t                                cfg = {0};
  config_setting_t                       *setting_pgw = NULL;
  config_setting_t                       *subsetting = NULL;
  config_setting_t                       *sub2setting = NULL;
  char                                   *if_S5_S8 = NULL;
  char                                   *if_SGI = NULL;
  char                                   *SGI = NULL;
  char                                   *masquerade_SGI = NULL;
  char                                   *ue_tcp_mss_clamping = NULL;
  char                                   *default_dns = NULL;
  char                                   *default_dns_sec = NULL;
  const char                             *astring = NULL;
  bstring                                 address = NULL;
  bstring                                 cidr = NULL;
  bstring                                 mask = NULL;
  int                                     num = 0;
  int                                     i = 0;
  unsigned char                           buf_in_addr[sizeof (struct in_addr)];
  struct in_addr                          addr_start;
  bstring                                 system_cmd = NULL;
  libconfig_int                           mtu = 0;
  int                                     prefix_mask = 0;
  struct in_addr                          in_addr_var = {0};


  config_init (&cfg);

  if (config_pP->config_file) {
    /*
     * Read the file. If there is an error, report it and exit.
     */
    if (!config_read_file (&cfg, bdata(config_pP->config_file))) {
      OAILOG_ERROR (LOG_SPGW_APP, "%s:%d - %s\n", bdata(config_pP->config_file), config_error_line (&cfg), config_error_text (&cfg));
      config_destroy (&cfg);
      AssertFatal (1 == 0, "Failed to parse SP-GW configuration file %s!\n", bdata(config_pP->config_file));
    }
  } else {
    OAILOG_ERROR (LOG_SPGW_APP, "No SP-GW configuration file provided!\n");
    config_destroy (&cfg);
    AssertFatal (0, "No SP-GW configuration file provided!\n");
  }

  OAILOG_INFO (LOG_SPGW_APP, "Parsing configuration file provided %s\n", bdata(config_pP->config_file));

  system_cmd = bfromcstr("");

  setting_pgw = config_lookup (&cfg, PGW_CONFIG_STRING_PGW_CONFIG);

  if (setting_pgw) {
    subsetting = config_setting_get_member (setting_pgw, PGW_CONFIG_STRING_NETWORK_INTERFACES_CONFIG);

    if (subsetting) {
      if ((config_setting_lookup_string (subsetting, PGW_CONFIG_STRING_PGW_INTERFACE_NAME_FOR_S5_S8, (const char **)&if_S5_S8)
           && config_setting_lookup_string (subsetting, PGW_CONFIG_STRING_PGW_INTERFACE_NAME_FOR_SGI, (const char **)&if_SGI)
           && config_setting_lookup_string (subsetting, PGW_CONFIG_STRING_PGW_IPV4_ADDRESS_FOR_SGI, (const char **)&SGI)
           && config_setting_lookup_string (subsetting, PGW_CONFIG_STRING_PGW_MASQUERADE_SGI, (const char **)&masquerade_SGI)
           && config_setting_lookup_string (subsetting, PGW_CONFIG_STRING_UE_TCP_MSS_CLAMPING, (const char **)&ue_tcp_mss_clamping)
          )
        ) {
        config_pP->ipv4.if_name_S5_S8 = bfromcstr(if_S5_S8);
        config_pP->ipv4.if_name_SGI = bfromcstr (if_SGI);
        cidr = bfromcstr (SGI);
        struct bstrList *list = bsplit (cidr, '/');
        AssertFatal(2 == list->qty, "Bad CIDR address %s", bdata(cidr));
        address = list->entry[0];
        mask    = list->entry[1];
        IPV4_STR_ADDR_TO_INADDR (bdata(address), config_pP->ipv4.SGI, "BAD IP ADDRESS FORMAT FOR S1u_S12_S4 !\n");
        config_pP->ipv4.mask_sgi = atoi ((const char*)mask->data);
        bstrListDestroy(list);
        in_addr_var.s_addr = config_pP->ipv4.SGI.s_addr;
        OAILOG_INFO (LOG_SPGW_APP, "Parsing configuration file found SGi: %s/%d on %s\n",
                       inet_ntoa (in_addr_var), config_pP->ipv4.mask_sgi, bdata(config_pP->ipv4.if_name_SGI));

#if ENABLE_LIBGTPNL
        if (strcasecmp (masquerade_SGI, "yes") == 0) {
          config_pP->masquerade_SGI = true;
          OAILOG_DEBUG (LOG_SPGW_APP, "Masquerade SGI\n");
        } else {
          config_pP->masquerade_SGI = false;
          OAILOG_DEBUG (LOG_SPGW_APP, "No masquerading for SGI\n");
        }
        if (strcasecmp (ue_tcp_mss_clamping, "yes") == 0) {
          config_pP->ue_tcp_mss_clamp = true;
          OAILOG_DEBUG (LOG_SPGW_APP, "CLAMP TCP MSS\n");
        } else {
          config_pP->ue_tcp_mss_clamp = false;
          OAILOG_DEBUG (LOG_SPGW_APP, "NO CLAMP TCP MSS\n");
        }
#endif
      } else {
        OAILOG_WARNING (LOG_SPGW_APP, "CONFIG P-GW / NETWORK INTERFACES parsing failed\n");
      }
    } else {
      OAILOG_WARNING (LOG_SPGW_APP, "CONFIG P-GW / NETWORK INTERFACES not found\n");
    }

    //!!!------------------------------------!!!
    subsetting = config_setting_get_member (setting_pgw, PGW_CONFIG_STRING_IP_ADDRESS_POOL);

    if (subsetting) {

      config_pP->arp_ue_oai = false;
      config_pP->arp_ue_linux = false;

      if (config_setting_lookup_string (subsetting, PGW_CONFIG_STRING_ARP_UE, (const char **)&astring)) {
        if (strcasecmp (astring, PGW_CONFIG_STRING_ARP_UE_CHOICE_LINUX) == 0) {
          config_pP->arp_ue_linux = true;
        } else if (strcasecmp (astring, PGW_CONFIG_STRING_ARP_UE_CHOICE_OAI) == 0) {
          // TODO
          config_pP->arp_ue_oai = true;
          AssertFatal(0, "ARP UE managed by OAI SPGW not available yet");
        }
      }

      sub2setting = config_setting_get_member (subsetting, PGW_CONFIG_STRING_IPV4_ADDRESS_LIST);

      if (sub2setting) {
        num = config_setting_length (sub2setting);

        for (i = 0; i < num; i++) {
          astring = config_setting_get_string_elem (sub2setting, i);

          if (astring) {
            cidr = bfromcstr (astring);
            AssertFatal(BSTR_OK == btrimws(cidr), "Error in PGW_CONFIG_STRING_IPV4_ADDRESS_LIST %s", astring);
            struct bstrList *list = bsplit (cidr, PGW_CONFIG_STRING_IPV4_PREFIX_DELIMITER);
            AssertFatal(2 == list->qty, "Bad CIDR address %s", bdata(cidr));

            address = list->entry[0];
            mask    = list->entry[1];

            if (inet_pton (AF_INET, bdata(address), buf_in_addr) == 1) {
              memcpy (&addr_start, buf_in_addr, sizeof (struct in_addr));
              // valid address
              prefix_mask = atoi ((const char *)mask->data);

              if ((prefix_mask >= 2) && (prefix_mask < 32) && (config_pP->num_ue_pool < PGW_NUM_UE_POOL_MAX)) {
                memcpy (&config_pP->ue_pool_addr[config_pP->num_ue_pool], buf_in_addr, sizeof (struct in_addr));
                config_pP->ue_pool_mask[config_pP->num_ue_pool] = prefix_mask;
                config_pP->num_ue_pool += 1;
              } else {
                OAILOG_ERROR (LOG_SPGW_APP, "CONFIG POOL ADDR IPV4: BAD MASQ: %d\n", prefix_mask);
              }
            }
            bstrListDestroy(list);
          }
        }
      } else {
        OAILOG_WARNING (LOG_SPGW_APP, "CONFIG POOL ADDR IPV4: NO IPV4 ADDRESS FOUND\n");
      }

      if (config_setting_lookup_string (setting_pgw, PGW_CONFIG_STRING_DEFAULT_DNS_IPV4_ADDRESS, (const char **)&default_dns)
          && config_setting_lookup_string (setting_pgw, PGW_CONFIG_STRING_DEFAULT_DNS_SEC_IPV4_ADDRESS, (const char **)&default_dns_sec)) {
        config_pP->ipv4.if_name_S5_S8 = bfromcstr (if_S5_S8);
        IPV4_STR_ADDR_TO_INADDR (default_dns, config_pP->ipv4.default_dns, "BAD IPv4 ADDRESS FORMAT FOR DEFAULT DNS !\n");
        IPV4_STR_ADDR_TO_INADDR (default_dns_sec, config_pP->ipv4.default_dns_sec, "BAD IPv4 ADDRESS FORMAT FOR DEFAULT DNS SEC!\n");
        OAILOG_DEBUG (LOG_SPGW_APP, "Parsing configuration file default primary DNS IPv4 address: %s\n", default_dns);
        OAILOG_DEBUG (LOG_SPGW_APP, "Parsing configuration file default secondary DNS IPv4 address: %s\n", default_dns_sec);
      } else {
        OAILOG_WARNING (LOG_SPGW_APP, "NO DNS CONFIGURATION FOUND\n");
      }
    }
    if (config_setting_lookup_string (setting_pgw, PGW_CONFIG_STRING_NAS_FORCE_PUSH_PCO, (const char **)&astring)) {
      if (strcasecmp (astring, "yes") == 0) {
        config_pP->force_push_pco = true;
        OAILOG_DEBUG (LOG_SPGW_APP, "Protocol configuration options: push MTU, push DNS, IP address allocation via NAS signalling\n");
      } else {
        config_pP->force_push_pco = false;
      }
    }
    if (config_setting_lookup_int (setting_pgw, PGW_CONFIG_STRING_UE_MTU, &mtu)) {
      config_pP->ue_mtu = mtu;
    } else {
      config_pP->ue_mtu = 1463;
    }
    OAILOG_DEBUG (LOG_SPGW_APP, "UE MTU : %u\n", config_pP->ue_mtu);
    if (config_setting_lookup_string (setting_pgw, PGW_CONFIG_STRING_GTPV1U_REALIZATION, (const char **)&astring)) {
      if (strcasecmp (astring, PGW_CONFIG_STRING_NO_GTP_KERNEL_AVAILABLE) == 0) {
        config_pP->use_gtp_kernel_module = false;
        config_pP->enable_loading_gtp_kernel_module = false;
        OAILOG_DEBUG (LOG_SPGW_APP, "Protocol configuration options: push MTU, push DNS, IP address allocation via NAS signalling\n");
      } else if (strcasecmp (astring, PGW_CONFIG_STRING_GTP_KERNEL_MODULE) == 0) {
        config_pP->use_gtp_kernel_module = true;
        config_pP->enable_loading_gtp_kernel_module = true;
      } else if (strcasecmp (astring, PGW_CONFIG_STRING_GTP_KERNEL) == 0) {
        config_pP->use_gtp_kernel_module = true;
        config_pP->enable_loading_gtp_kernel_module = false;
      }
    }


    subsetting = config_setting_get_member (setting_pgw, PGW_CONFIG_STRING_PCEF);
    if (subsetting) {
      if ((config_setting_lookup_string (subsetting, PGW_CONFIG_STRING_PCEF_ENABLED, (const char **)&astring))) {
        if (strcasecmp (astring, "yes") == 0) {
          config_pP->pcef.enabled = true;

          if (config_setting_lookup_string (subsetting, PGW_CONFIG_STRING_TCP_ECN_ENABLED, (const char **)&astring)) {
            if (strcasecmp (astring, "yes") == 0) {
              config_pP->pcef.tcp_ecn_enabled = true;
              OAILOG_DEBUG (LOG_SPGW_APP, "TCP ECN enabled\n");
            } else {
              config_pP->pcef.tcp_ecn_enabled = false;
            }
          }

          libconfig_int sdf_id = 0;
          if (config_setting_lookup_int (subsetting, PGW_CONFIG_STRING_AUTOMATIC_PUSH_DEDICATED_BEARER_PCC_RULE, &sdf_id)) {
            AssertFatal((sdf_id < SDF_ID_MAX) && (sdf_id >= 0), "Bad SDF identifier value %d for dedicated bearer", sdf_id);
            config_pP->pcef.automatic_push_dedicated_bearer_sdf_identifier = sdf_id;
          }

          if (config_setting_lookup_int (subsetting, PGW_CONFIG_STRING_DEFAULT_BEARER_STATIC_PCC_RULE, &sdf_id)) {
            AssertFatal((sdf_id < SDF_ID_MAX) && (sdf_id >= 0), "Bad SDF identifier value %d for default bearer", sdf_id);
            config_pP->pcef.default_bearer_sdf_identifier = sdf_id;
          }

          sub2setting = config_setting_get_member (subsetting, PGW_CONFIG_STRING_PUSH_STATIC_PCC_RULES);
          if (sub2setting != NULL) {
            num = config_setting_length (sub2setting);

            AssertFatal(num <= (SDF_ID_MAX - 1), "Too many PCC rules defined (%d>%d)", num, SDF_ID_MAX);

            for (i = 0; i < num; i++) {
              config_pP->pcef.preload_static_sdf_identifiers[i] = config_setting_get_int_elem(sub2setting, i);
            }
          }

          libconfig_int apn_ambr_ul = 0;
          if (config_setting_lookup_int (subsetting, PGW_CONFIG_STRING_APN_AMBR_UL, &apn_ambr_ul)) {
            AssertFatal((0 < apn_ambr_ul), "Bad APN AMBR UL value %d", apn_ambr_ul);
            config_pP->pcef.apn_ambr_ul = apn_ambr_ul;
          } else {
            config_pP->pcef.apn_ambr_ul = 50000;
          }
          libconfig_int apn_ambr_dl = 0;
          if (config_setting_lookup_int (subsetting, PGW_CONFIG_STRING_APN_AMBR_DL, &apn_ambr_dl)) {
            AssertFatal((0 < apn_ambr_dl), "Bad APN AMBR DL value %d", apn_ambr_dl);
            config_pP->pcef.apn_ambr_dl = apn_ambr_dl;
          } else {
            config_pP->pcef.apn_ambr_dl = 50000;
          }
        } else {
          config_pP->pcef.enabled = false;
        }
      } else {
        OAILOG_WARNING (LOG_SPGW_APP, "CONFIG P-GW / %s parsing failed\n", PGW_CONFIG_STRING_PCEF);
      }
    } else {
      OAILOG_WARNING (LOG_SPGW_APP, "CONFIG P-GW / %s not found\n", PGW_CONFIG_STRING_PCEF);
    }
  } else {
    OAILOG_WARNING (LOG_SPGW_APP, "CONFIG P-GW not found\n");
  }
  bdestroy_wrapper (&system_cmd);
  config_destroy (&cfg);
  return RETURNok;
}

//------------------------------------------------------------------------------
void pgw_config_display (pgw_config_t * config_p)
{
  OAILOG_INFO (LOG_SPGW_APP, "==== EURECOM %s v%s ====\n", PACKAGE_NAME, PACKAGE_VERSION);
  OAILOG_INFO (LOG_SPGW_APP, "Configuration:\n");
  OAILOG_INFO (LOG_SPGW_APP, "- File .................................: %s\n", bdata(config_p->config_file));

  OAILOG_INFO (LOG_SPGW_APP, "- S5-S8:\n");
  OAILOG_INFO (LOG_SPGW_APP, "    S5_S8 iface ..........: %s\n", bdata(config_p->ipv4.if_name_S5_S8));
  OAILOG_INFO (LOG_SPGW_APP, "    S5_S8 ip  (read)......: %s\n", inet_ntoa (*((struct in_addr *)&config_p->ipv4.S5_S8)));
  OAILOG_INFO (LOG_SPGW_APP, "    S5_S8 MTU (read)......: %u\n", config_p->ipv4.mtu_S5_S8);
  OAILOG_INFO (LOG_SPGW_APP, "- SGi:\n");
  OAILOG_INFO (LOG_SPGW_APP, "    SGi iface ............: %s\n", bdata(config_p->ipv4.if_name_SGI));
  OAILOG_INFO (LOG_SPGW_APP, "    SGi ip  (read)........: %s\n", inet_ntoa (*((struct in_addr *)&config_p->ipv4.SGI)));
  OAILOG_INFO (LOG_SPGW_APP, "    SGi MTU (read)........: %u\n", config_p->ipv4.mtu_SGI);
#if ENABLE_LIBGTPNL
  OAILOG_INFO (LOG_SPGW_APP, "    User TCP MSS clamping : %s\n", config_p->ue_tcp_mss_clamp == 0 ? "false" : "true");
  OAILOG_INFO (LOG_SPGW_APP, "    User IP masquerading  : %s\n", config_p->masquerade_SGI == 0 ? "false" : "true");
#endif
  if (config_p->use_gtp_kernel_module) {
    OAILOG_INFO (LOG_SPGW_APP, "- GTPv1U .................: Enabled (Linux kernel module)\n");
    OAILOG_INFO (LOG_SPGW_APP, "    Load/unload module....: %s\n", (config_p->enable_loading_gtp_kernel_module) ? "enabled" : "disabled");
  } else {
    OAILOG_INFO (LOG_SPGW_APP, "- GTPv1U .................: Disabled\n");
  }

  OAILOG_INFO (LOG_SPGW_APP, "- PCEF support ...........: %s (in development)\n", config_p->pcef.enabled == 0 ? "false" : "true");
  if (config_p->pcef.enabled) {
    OAILOG_INFO (LOG_SPGW_APP, "    TCP ECN  .............: %s\n", config_p->pcef.tcp_ecn_enabled == 0 ? "false" : "true");
    OAILOG_INFO (LOG_SPGW_APP, "    Push dedicated bearer SDF ID: %d (testing dedicated bearer functionality down to OAI UE/COSTS UE)\n",
        config_p->pcef.automatic_push_dedicated_bearer_sdf_identifier);
    OAILOG_INFO (LOG_SPGW_APP, "    Default bearer SDF ID.: %d\n",config_p->pcef.default_bearer_sdf_identifier);
    bstring pcc_rules= bfromcstralloc(64, "(");
    for (int i = 0; i < (SDF_ID_MAX-1); i++) {
      if (i == 0) {
        bformata(pcc_rules, " %u", config_p->pcef.preload_static_sdf_identifiers[i]);
        if (!config_p->pcef.preload_static_sdf_identifiers[i]) {
          break;
        }
      } else {
        if (config_p->pcef.preload_static_sdf_identifiers[i]) {
          bformata(pcc_rules, ", %u", config_p->pcef.preload_static_sdf_identifiers[i]);
        } else
          break;
      }
    }
    bcatcstr(pcc_rules, " )");
    OAILOG_INFO (LOG_SPGW_APP, "    Preloaded static PCC Rules.: %s\n", bdata(pcc_rules));
    bdestroy_wrapper(&pcc_rules);


    OAILOG_INFO (LOG_SPGW_APP, "    APN AMBR UL ..........: %"PRIu64" (Kilo bits/s)\n", config_p->pcef.apn_ambr_ul);
    OAILOG_INFO (LOG_SPGW_APP, "    APN AMBR DL ..........: %"PRIu64" (Kilo bits/s)\n", config_p->pcef.apn_ambr_dl);
  }
  OAILOG_INFO (LOG_SPGW_APP, "- Helpers:\n");
  OAILOG_INFO (LOG_SPGW_APP, "    Push PCO (DNS+MTU) ........: %s\n", config_p->force_push_pco == 0 ? "false" : "true");
}

#ifdef __cplusplus
}
#endif
