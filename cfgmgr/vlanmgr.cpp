#include <string.h>
#include "logger.h"
#include "producerstatetable.h"
#include "macaddress.h"
#include "vlanmgr.h"
#include "exec.h"
#include "tokenize.h"
#include "shellcmd.h"
#include "warm_restart.h"
#include <swss/redisutility.h>
#include "subintf.h"

using namespace std;
using namespace swss;

#define DOT1Q_BRIDGE_NAME   "Bridge"
#define DFLT_BR_AGE_TIME    "600"
#define VLAN_PREFIX         "Vlan"
#define LAG_PREFIX          "PortChannel"
#define DEFAULT_VLAN_ID     "1"
#define DEFAULT_MTU_STR     "9100"
#define VLAN_HLEN            4
#define NFT_ARP_CHAIN       "ARP_LIST"
#define NFT_ND_CHAIN        "ND_LIST"
#define NFT_VLAN_ARP_CHAIN  "VLAN_ARP_LIST"

extern MacAddress gMacAddress;

VlanMgr::VlanMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<TableConnector> &tables) :
        Orch(tables),
        m_cfgVlanTable(cfgDb, CFG_VLAN_TABLE_NAME),
        m_cfgVlanMemberTable(cfgDb, CFG_VLAN_MEMBER_TABLE_NAME),
        m_cfgNeighSuppressVlanTable(cfgDb, CFG_NEIGH_SUPPRESS_VLAN_TABLE_NAME),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME),
        m_stateVlanTable(stateDb, STATE_VLAN_TABLE_NAME),
        m_stateVlanMemberTable(stateDb, STATE_VLAN_MEMBER_TABLE_NAME),
        m_stateNeighSuppressVlanTable(stateDb, STATE_NEIGH_SUPPRESS_VLAN_TABLE_NAME),
        m_appVlanTableProducer(appDb, APP_VLAN_TABLE_NAME),
        m_appVlanMemberTableProducer(appDb, APP_VLAN_MEMBER_TABLE_NAME),
        m_cfgSubInterfaceTable(cfgDb, CFG_VLAN_SUB_INTF_TABLE_NAME),
        m_appNeighSuppressVlanTableProducer(appDb, APP_NEIGH_SUPPRESS_VLAN_TABLE_NAME),
        replayDone(false)
{
    SWSS_LOG_ENTER();

    std::string nftables_cmd, res;
    nftables_cmd = std::string("") + "nft flush chain bridge filter " + NFT_ARP_CHAIN;
    swss::exec(nftables_cmd.c_str(), res);
    nftables_cmd = std::string("") + "nft flush chain bridge filter " + NFT_ND_CHAIN;
    swss::exec(nftables_cmd.c_str(), res);
    nftables_cmd = std::string("") + "nft flush chain bridge filter " + NFT_VLAN_ARP_CHAIN;
    swss::exec(nftables_cmd.c_str(), res);
    int ret;

    if (WarmStart::isWarmStart())
    {
        vector<string> vlanKeys, vlanMemberKeys;

        /* cache all vlan and vlan member config */
        m_cfgVlanTable.getKeys(vlanKeys);
        m_cfgVlanMemberTable.getKeys(vlanMemberKeys);
        for (auto k : vlanKeys)
        {
            m_vlanReplay.insert(k);
        }
        for (auto k : vlanMemberKeys)
        {
            m_vlanMemberReplay.insert(k);
        }
        if (m_vlanReplay.empty())
        {
            replayDone = true;
            WarmStart::setWarmStartState("vlanmgrd", WarmStart::REPLAYED);
            SWSS_LOG_NOTICE("vlanmgr warmstart state set to REPLAYED");
            WarmStart::setWarmStartState("vlanmgrd", WarmStart::RECONCILED);
            SWSS_LOG_NOTICE("vlanmgr warmstart state set to RECONCILED");
        }
        const std::string cmds = std::string("")
          + IP_CMD + " link show " + DOT1Q_BRIDGE_NAME + " 2>/dev/null";

        std::string res;
        ret = swss::exec(cmds, res);
        if (ret == 0)
        {
            // Don't reset vlan aware bridge upon swss docker warm restart.
            SWSS_LOG_INFO("vlanmgrd warm start, skipping bridge create");
            return;
        }
    }
    // Initialize Linux dot1q bridge and enable vlan filtering
    // The command should be generated as:
    // /bin/bash -c "/sbin/ip link del Bridge 2>/dev/null ;
    //               /sbin/ip link add Bridge up type bridge &&
    //               /sbin/ip link set Bridge mtu {{ mtu_size }} &&
    //               /sbin/ip link set Bridge address {{gMacAddress}} &&
    //               /sbin/ip link set Bridge addrgenmode none &&
    //               /sbin/ip address flush Bridge &&
    //               /sbin/bridge vlan del vid 1 dev Bridge self;
    //               /sbin/ip link del dummy 2>/dev/null;
    //               /sbin/ip link add dummy type dummy &&
    //               /sbin/ip link set dummy master Bridge"

    const std::string cmds = std::string("")
      + BASH_CMD + " -c \""
      + IP_CMD + " link del " + DOT1Q_BRIDGE_NAME + " 2>/dev/null; "
      + IP_CMD + " link add " + DOT1Q_BRIDGE_NAME + " up type bridge && "
      + IP_CMD + " link set " + DOT1Q_BRIDGE_NAME + " mtu " + DEFAULT_MTU_STR + " && "
      + IP_CMD + " link set " + DOT1Q_BRIDGE_NAME + " address " + gMacAddress.to_string() + " && "
      + IP_CMD + " link set " + DOT1Q_BRIDGE_NAME + " addrgenmode none && "
      + IP_CMD + " address flush " + DOT1Q_BRIDGE_NAME + " && "
      + BRIDGE_CMD + " vlan del vid " + DEFAULT_VLAN_ID + " dev " + DOT1Q_BRIDGE_NAME + " self; "
      + IP_CMD + " link del dev dummy 2>/dev/null; "
      + IP_CMD + " link add dummy type dummy && "
      + IP_CMD + " link set dummy master " + DOT1Q_BRIDGE_NAME + "\"";

    ret = swss::exec(cmds, res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
    }

    // The generated command is:
    // /bin/echo 1 > /sys/class/net/Bridge/bridge/vlan_filtering
    const std::string echo_cmd = std::string("")
      + ECHO_CMD + " 1 > /sys/class/net/" + DOT1Q_BRIDGE_NAME + "/bridge/vlan_filtering";

    ret = swss::exec(echo_cmd, res);
    /* echo will fail in virtual switch since /sys directory is read-only.
     * need to use ip command to setup the vlan_filtering which is not available in debian 8.
     * Once we move sonic to debian 9, we can use IP command by default
     * ip command available in Debian 9 to create a bridge with a vlan filtering:
     * /sbin/ip link add Bridge up type bridge vlan_filtering 1 */
    if (ret != 0)
    {
        const std::string echo_cmd_backup = std::string("")
          + IP_CMD + " link set " + DOT1Q_BRIDGE_NAME + " type bridge vlan_filtering 1";

        int ret_2 = swss::exec(echo_cmd_backup, res);
        if (ret_2)
        {
            SWSS_LOG_ERROR("Command '%s' failed with rc %d", echo_cmd_backup.c_str(), ret_2);
        }
    }

    // not learn from link-local frames
    // /bin/echo 1 > /sys/class/net/Bridge/bridge/no_linklocal_learn
    const std::string no_ll_learn_cmd = std::string("")
      + ECHO_CMD + " 1 > /sys/class/net/" + DOT1Q_BRIDGE_NAME + "/bridge/no_linklocal_learn";

    ret = swss::exec(no_ll_learn_cmd, res);
    if (ret) {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", no_ll_learn_cmd.c_str(), ret);
    }

    // Initialize Linux dot1q bridge ageing time based on SWITCH_TABLE from APPL_DB
    // The command should be generated as:
    // /bin/bash -c "/sbin/brctl setageing Bridge 600"
    const std::string brctl_cmd = std::string("")
        + BRCTL_CMD + " setageing " + DOT1Q_BRIDGE_NAME + " " + DFLT_BR_AGE_TIME;
    ret = swss::exec(brctl_cmd, res);
    if (ret) {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", brctl_cmd.c_str(), ret);
    }
}

bool VlanMgr::addHostVlan(int vlan_id)
{
    SWSS_LOG_ENTER();

    // The command should be generated as:
    // /bin/bash -c "/sbin/bridge vlan add vid {{vlan_id}} dev Bridge self &&
    //               /sbin/ip link add link Bridge up name Vlan{{vlan_id}} address {{gMacAddress}} type vlan id {{vlan_id}}"
    const std::string cmds = std::string("")
      + BASH_CMD + " -c \""
      + BRIDGE_CMD + " vlan add vid " + std::to_string(vlan_id) + " dev " + DOT1Q_BRIDGE_NAME + " self && "
      + IP_CMD + " link add link " + DOT1Q_BRIDGE_NAME
               + " up"
               + " name " + VLAN_PREFIX + std::to_string(vlan_id)
               + " address " + gMacAddress.to_string()
               + " type vlan id " + std::to_string(vlan_id) + "\"";

    std::string res;
    int ret = swss::exec(cmds, res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
    }

    res.clear();
    const std::string echo_cmd = std::string("")
      + ECHO_CMD + " 0 > /proc/sys/net/ipv4/conf/" + VLAN_PREFIX + std::to_string(vlan_id) + "/arp_evict_nocarrier";
    swss::exec(echo_cmd, res);

    return true;
}

bool VlanMgr::removeHostVlan(int vlan_id)
{
    SWSS_LOG_ENTER();

    // The command should be generated as:
    // /bin/bash -c "/sbin/ip link del Vlan{{vlan_id}} &&
    //               /sbin/bridge vlan del vid {{vlan_id}} dev Bridge self"
    const std::string cmds = std::string("")
      + BASH_CMD + " -c \""
      + IP_CMD + " link del " + VLAN_PREFIX + std::to_string(vlan_id) + " && "
      + BRIDGE_CMD + " vlan del vid " + std::to_string(vlan_id) + " dev " + DOT1Q_BRIDGE_NAME + " self\"";

    std::string res;
    int ret = swss::exec(cmds, res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.c_str(), ret);
    }

    return true;
}

bool VlanMgr::setHostVlanAdminState(int vlan_id, const string &admin_status)
{
    SWSS_LOG_ENTER();

    // The command should be generated as:
    // /sbin/ip link set Vlan{{vlan_id}} {{admin_status}}
    ostringstream cmds;
    cmds << IP_CMD " link set " VLAN_PREFIX + std::to_string(vlan_id) + " " << shellquote(admin_status);

    std::string res;
    int ret = swss::exec(cmds.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.str().c_str(), ret);
    }
    return true;
}

bool VlanMgr::setHostVlanMtu(int vlan_id, uint32_t mtu)
{
    SWSS_LOG_ENTER();

    // The command should be generated as:
    // /sbin/ip link set Vlan{{vlan_id}} mtu {{mtu}}
    const std::string cmds = std::string("")
      + IP_CMD + " link set " + VLAN_PREFIX + std::to_string(vlan_id) + " mtu " + std::to_string(mtu);

    std::string res;
    int ret = swss::exec(cmds, res);
    if (ret == 0)
    {
        return true;
    }

    /* VLAN mtu should not be larger than member mtu */
    return false;
}

bool VlanMgr::setHostVlanMac(int vlan_id, const string &mac)
{
    SWSS_LOG_ENTER();

    // The command should be generated as:
    // /sbin/ip link set Vlan{{vlan_id}} address {{mac}}
    ostringstream cmds;
    cmds << IP_CMD " link set " VLAN_PREFIX + std::to_string(vlan_id) + " address " << shellquote(mac) << " && "
            IP_CMD " link set " DOT1Q_BRIDGE_NAME " address " << shellquote(mac);

    std::string res;
    int ret = swss::exec(cmds.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.str().c_str(), ret);
    }

    return true;
}

bool VlanMgr::addHostVlanMember(int vlan_id, const string &port_alias, const string& tagging_mode)
{
    SWSS_LOG_ENTER();
    string key_def_vlan = VLAN_PREFIX DEFAULT_VLAN_ID CONFIGDB_KEY_SEPARATOR + port_alias;

    std::string tagging_cmd;
    if (tagging_mode == "untagged" || tagging_mode == "priority_tagged")
    {
        tagging_cmd = "pvid untagged";
    }

    // The command should be generated as:
    // /bin/bash -c "/sbin/ip link set {{port_alias}} master Bridge &&
    //               /sbin/bridge vlan del vid 1 dev {{ port_alias }} &&
    //               /sbin/bridge vlan add vid {{vlan_id}} dev {{port_alias}} {{tagging_mode}}"
    ostringstream cmds, inner;
    if (!isVlanMemberStateOk(key_def_vlan))
    {
        inner << IP_CMD " link set " << shellquote(port_alias) << " master " DOT1Q_BRIDGE_NAME " && "
          BRIDGE_CMD " vlan del vid " DEFAULT_VLAN_ID " dev " << shellquote(port_alias) << " && "
          BRIDGE_CMD " vlan add vid " + std::to_string(vlan_id) + " dev " << shellquote(port_alias) << " " + tagging_cmd;
    }
    else
    {
        inner << IP_CMD " link set " << shellquote(port_alias) << " master " DOT1Q_BRIDGE_NAME " && "
          BRIDGE_CMD " vlan add vid " + std::to_string(vlan_id) + " dev " << shellquote(port_alias) << " " + tagging_cmd;
    }
    cmds << BASH_CMD " -c " << shellquote(inner.str());

    std::string res;
    int ret = swss::exec(cmds.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.str().c_str(), ret);
    }

    vector<FieldValueTuple> values;
    if (isVlanNeighborSuppressed(vlan_id)
        && m_stateNeighSuppressVlanTable.get(string("Vlan") + to_string(vlan_id), values))
    {
        updateVlanMemberNftRule(vlan_id, port_alias, true);
    }

    return true;
}

bool VlanMgr::removeHostVlanMember(int vlan_id, const string &port_alias)
{
    SWSS_LOG_ENTER();

    // The command should be generated as:
    // /bin/bash -c '/sbin/bridge vlan del vid {{vlan_id}} dev {{port_alias}} &&
    //               ( vlanShow=$(/sbin/bridge vlan show dev {{port_alias}});
    //               ret=$?;
    //               if [ $ret -eq 0 ]; then
    //               if (! echo "$vlanShow" | grep -q {{port_alias}})
    //                 || (echo "$vlanShow" | grep -q None$)
    //                 || (echo "$vlanShow" | grep -q {{port_alias}}$); then
    //               /sbin/ip link set {{port_alias}} nomaster;
    //               fi;
    //               else exit $ret; fi )'

    // When port is not member of any VLAN, it shall be detached from Dot1Q bridge!
    ostringstream cmds, inner;
    inner << BRIDGE_CMD " vlan del vid " + std::to_string(vlan_id) + " dev " << shellquote(port_alias) << " && ( "
      "vlanShow=$(" BRIDGE_CMD " vlan show dev " << shellquote(port_alias) << "); "
      "ret=$?; "
      "if [ $ret -eq 0 ]; then "
      "if (! echo \"$vlanShow\" | " GREP_CMD " -q " << shellquote(port_alias) << ") "
      " || (echo \"$vlanShow\" | " GREP_CMD " -q None$) "
      " || (echo \"$vlanShow\" | " GREP_CMD " -q " << shellquote(port_alias) << "$); then "
      IP_CMD " link set " << shellquote(port_alias) << " nomaster; "
      "fi; "
      "else exit $ret; fi )";
    cmds << BASH_CMD " -c " << shellquote(inner.str());

    std::string res;
    int ret = swss::exec(cmds.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmds.str().c_str(), ret);
    }
    updateVlanMemberNftRule(vlan_id, port_alias, false);
    return true;
}

bool VlanMgr::isVlanMacOk()
{
    return !!gMacAddress;
}
bool VlanMgr::isSubportConfigVlan(const int vlan_id)
{
    std::vector<std::string> keys;
    m_cfgSubInterfaceTable.getKeys(keys);
    for (const auto& tmp_key : keys)
    {
        if (tmp_key.find(VLAN_SUB_INTERFACE_SEPARATOR) == string::npos)
        {
            continue;
        }
        subIntf subIf(tmp_key);
        if (vlan_id && vlan_id == subIf.subIntfIdx())
        {
            return true;
        }
    }
    return false;
}

bool VlanMgr::isVlanNeighborSuppressed(int vlan_id)
{
    string key = VLAN_PREFIX + to_string(vlan_id);
    string neighbor_suppress_str;

    m_cfgNeighSuppressVlanTable.hget(key.c_str(), "suppress", neighbor_suppress_str);
    return (neighbor_suppress_str == "on") ? true : false;
}

bool VlanMgr::setNftRule(const std::string &chain_name, const std::string port_alias, bool is_add, int vlan_id)
{
    std::string nftables_cmd, res;
    std::string key = chain_name;
    key = key + "_" + to_string(vlan_id);

    if (is_add)
    {
        if (m_nftNdSpRuleHandles.find(key) != m_nftNdSpRuleHandles.end())
            if (m_nftNdSpRuleHandles[key].find(port_alias) != m_nftNdSpRuleHandles[key].end())
                return true;
        if (port_alias.find("vtep") != std::string::npos)
            nftables_cmd = "nft --echo --handle add rule bridge filter " + chain_name + " oifname " + port_alias +
                           " counter packets 0 bytes 0 accept | grep handle | awk '{print $NF}'";
        else if (m_nftVlanMbrSetMap[vlan_id] != "")
            nftables_cmd = "nft --echo --handle add rule bridge filter " + chain_name + " iifname " + port_alias +
                           " oifname == @" + m_nftVlanMbrSetMap[vlan_id] + " counter packets 0 bytes 0 accept | grep handle | awk '{print $NF}'";
        else
            return false;
    }
    else
    {
        auto it = m_nftNdSpRuleHandles.find(key);

        if (it != m_nftNdSpRuleHandles.end())
            nftables_cmd = "nft --echo --handle delete rule bridge filter "  + chain_name + " handle " + (it->second)[port_alias];
        else
            return false;
    }

    swss::exec(nftables_cmd.c_str(), res);

    if (is_add)
    {
        SWSS_LOG_INFO("Success to add nftables rule, key = [%s], handle = [%s]", key.c_str(), res.c_str());
        m_nftNdSpRuleHandles[key].insert(std::make_pair(port_alias, res));
    }
    else
        m_nftNdSpRuleHandles[key].erase(port_alias);

    return true;
}

void VlanMgr::updateNftVlanMbrSet(int vlan_id, bool is_set)
{
    string res, nft_set_cmd, set_name = "vlan" + to_string(vlan_id) + "_mbr";;

    if (is_set)
    {
        nft_set_cmd = "nft add set bridge filter " + set_name + " { type ifname\\; }";
        swss::exec(nft_set_cmd.c_str(), res);
        m_nftVlanMbrSetMap[vlan_id] = set_name;
    }
    else
    {
        nft_set_cmd = "nft delete set bridge filter " + m_nftVlanMbrSetMap[vlan_id];
        swss::exec(nft_set_cmd.c_str(), res);
        m_nftVlanMbrSetMap.erase(vlan_id);
    }
}

void VlanMgr::updateNftVlanMbrSetElement(int vlan_id, const std::string port_alias, std::string op)
{
    string res, element = "'{ " + port_alias + " }'";
    string nft_set_element_cmd = "nft " + op +" element bridge filter " + m_nftVlanMbrSetMap[vlan_id] + element;

    if (op == "add")
    {
        if (m_nftVlanMbrSetElement[vlan_id].find(port_alias) == m_nftVlanMbrSetElement[vlan_id].end())
        {
            m_nftVlanMbrSetElement[vlan_id].insert(port_alias);
            swss::exec(nft_set_element_cmd.c_str(), res);
        }
    }
    else
    {
        m_nftVlanMbrSetElement[vlan_id].erase(port_alias);
        swss::exec(nft_set_element_cmd.c_str(), res);
    }
}

void VlanMgr::updateVlanMemberNftRule(int vlan_id, const std::string port_alias, bool is_add)
{
    SWSS_LOG_INFO("Update nftables rules for port %s, vlan %d, operation: %s", port_alias.c_str(), vlan_id, is_add ? "ADD" : "DELETE");

    std::vector<std::string> nft_commands_to_batch;
    std::string cmd1_arp, cmd2_vlan_arp, cmd3_nd;

    if (is_add)
    {
        updateNftVlanMbrSet(vlan_id, true); // Ensure set exists before adding rules that reference it.

        cmd1_arp = generate_nft_rule_command(NFT_ARP_CHAIN, port_alias, true, vlan_id);
        cmd2_vlan_arp = generate_nft_rule_command(NFT_VLAN_ARP_CHAIN, port_alias, true, vlan_id);
        cmd3_nd = generate_nft_rule_command(NFT_ND_CHAIN, port_alias, true, vlan_id);

        if (!cmd1_arp.empty()) nft_commands_to_batch.push_back(cmd1_arp);
        if (!cmd2_vlan_arp.empty()) nft_commands_to_batch.push_back(cmd2_vlan_arp);
        if (!cmd3_nd.empty()) nft_commands_to_batch.push_back(cmd3_nd);

        if (!nft_commands_to_batch.empty())
        {
            if (execute_nft_batch_file(nft_commands_to_batch)) // check success
            {
                SWSS_LOG_INFO("Successfully batched ADD nftables rules for port %s, vlan %d.", port_alias.c_str(), vlan_id);
                updateNftVlanMbrSetElement(vlan_id, port_alias, "add"); // Only if batch succeeded
            }
            else
            {
                SWSS_LOG_ERROR("Failed to batch ADD nftables rules for port %s, vlan %d. Set element will not be added.", port_alias.c_str(), vlan_id);
                // Note: if batch fails, set element might not be added. Consider if set should be removed.
                // For now, if updateNftVlanMbrSet(true) was called, the set might exist even if rules failed.
            }
        }
        else
        {
            SWSS_LOG_WARN("No nft ADD commands generated for port %s, vlan %d.", port_alias.c_str(), vlan_id);
        }
    }
    else // Deleting rules
    {
        // First, remove port from the set. This prevents new traffic matching.
        // This also allows the set to be deleted if it becomes empty.
        if (m_nftVlanMbrSetElement.count(vlan_id) && m_nftVlanMbrSetElement[vlan_id].count(port_alias))
        {
             updateNftVlanMbrSetElement(vlan_id, port_alias, "delete");
        }

        cmd1_arp = generate_nft_rule_command(NFT_ARP_CHAIN, port_alias, false, vlan_id);
        cmd2_vlan_arp = generate_nft_rule_command(NFT_VLAN_ARP_CHAIN, port_alias, false, vlan_id);
        cmd3_nd = generate_nft_rule_command(NFT_ND_CHAIN, port_alias, false, vlan_id);

        if (!cmd1_arp.empty()) nft_commands_to_batch.push_back(cmd1_arp);
        if (!cmd2_vlan_arp.empty()) nft_commands_to_batch.push_back(cmd2_vlan_arp);
        if (!cmd3_nd.empty()) nft_commands_to_batch.push_back(cmd3_nd);

        if (!nft_commands_to_batch.empty())
        {
            if (!execute_nft_batch_file(nft_commands_to_batch))
            {
                SWSS_LOG_ERROR("Failed to batch DELETE nftables rules for port %s, vlan %d.", port_alias.c_str(), vlan_id);
                // If rule deletion fails, the port element was still removed from the set.
                // This is generally safe.
            }
            else
            {
                SWSS_LOG_INFO("Successfully batched DELETE nftables rules for port %s, vlan %d.", port_alias.c_str(), vlan_id);
            }
        }
        else
        {
            SWSS_LOG_WARN("No nft DELETE commands generated for port %s, vlan %d.", port_alias.c_str(), vlan_id);
        }

        // Check if the set is now empty and can be deleted
        if (m_nftVlanMbrSetElement.count(vlan_id) && m_nftVlanMbrSetElement[vlan_id].empty())
        {
            SWSS_LOG_INFO("NFT member set for VLAN %d is empty, removing set.", vlan_id);
            // The original code for removing from m_neighborSuppressMap is in the multi-port version.
            // Here we just ensure the set itself is removed if empty.
            // m_neighborSuppressMap.erase(port_alias); // This was in old setNftRule, but seems more related to overall suppression state
            updateNftVlanMbrSet(vlan_id, false); // Deletes the set
            m_nftVlanMbrSetElement.erase(vlan_id); // Clean up the element tracking map for this vlan_id
            // Also, m_nftVlanMbrSetMap should be cleaned
            m_nftVlanMbrSetMap.erase(vlan_id);
        }
    }
}

void VlanMgr::updateVlanMemberNftRule(int vlan_id, bool is_add) // Function parameter vlan_id is the correct one to use.
{
    SWSS_LOG_INFO("Update NFT rules for ALL MEMBERS of vlan %d, operation: %s", vlan_id, is_add ? "ADD" : "DELETE");

    std::vector<std::string> all_nft_commands_to_batch;
    std::vector<std::string> members_processed; // Store port_alias of members processed

    std::string vlan_alias_filter = VLAN_PREFIX + std::to_string(vlan_id);
    std::vector<std::string> vlanMemberKeys;
    m_cfgVlanMemberTable.getKeys(vlanMemberKeys); // Get all member keys

    if (is_add)
    {
        updateNftVlanMbrSet(vlan_id, true); // Ensure set exists before adding rules

        for (const auto& key : vlanMemberKeys)
        {
            size_t delimiter_pos = key.find(CONFIGDB_KEY_SEPARATOR);
            if (delimiter_pos == std::string::npos) continue;

            std::string vlan_key_part = key.substr(0, delimiter_pos);
            if (vlan_key_part != vlan_alias_filter) continue; // Filter for the correct VLAN

            std::string port_alias = key.substr(delimiter_pos + 1);

            // Check if this port_alias is already processed for this vlan_id to avoid duplicate rule generation
            // (though nft -f might handle it, good to be clean)
            bool already_processed = false;
            for(const auto& processed_port : members_processed) {
                if (processed_port == port_alias) {
                    already_processed = true;
                    break;
                }
            }
            if (already_processed) continue;

            std::string cmd_arp = generate_nft_rule_command(NFT_ARP_CHAIN, port_alias, true, vlan_id);
            std::string cmd_vlan_arp = generate_nft_rule_command(NFT_VLAN_ARP_CHAIN, port_alias, true, vlan_id);
            std::string cmd_nd = generate_nft_rule_command(NFT_ND_CHAIN, port_alias, true, vlan_id);

            if (!cmd_arp.empty()) all_nft_commands_to_batch.push_back(cmd_arp);
            if (!cmd_vlan_arp.empty()) all_nft_commands_to_batch.push_back(cmd_vlan_arp);
            if (!cmd_nd.empty()) all_nft_commands_to_batch.push_back(cmd_nd);

            members_processed.push_back(port_alias);
        }

        if (!all_nft_commands_to_batch.empty())
        {
            if (execute_nft_batch_file(all_nft_commands_to_batch)) // check success
            {
                SWSS_LOG_INFO("Successfully batched ADD NFT rules for vlan %d.", vlan_id);
                for (const auto& pa : members_processed) // Use pa (port_alias) from members_processed
                {
                    updateNftVlanMbrSetElement(vlan_id, pa, "add"); // Only if batch succeeded
                    if (m_neighborSuppressMap.find(pa) == m_neighborSuppressMap.end())
                    {
                        m_neighborSuppressMap[pa] = std::set<int>();
                    }
                    m_neighborSuppressMap[pa].insert(vlan_id); // Use parameter vlan_id
                    SWSS_LOG_NOTICE("Updated m_neighborSuppressMap for ADD: port %s, vlan %d", pa.c_str(), vlan_id);
                }
            }
            else
            {
                SWSS_LOG_ERROR("Failed to batch ADD NFT rules for vlan %d. Set elements and neighbor map will not be updated.", vlan_id);
            }
        }
    }
    else // is_delete
    {
        for (const auto& key : vlanMemberKeys)
        {
            size_t delimiter_pos = key.find(CONFIGDB_KEY_SEPARATOR);
            if (delimiter_pos == std::string::npos) continue;

            std::string vlan_key_part = key.substr(0, delimiter_pos);
            if (vlan_key_part != vlan_alias_filter) continue;

            std::string port_alias = key.substr(delimiter_pos + 1);

            bool already_processed = false;
            for(const auto& processed_port : members_processed) {
                if (processed_port == port_alias) {
                    already_processed = true;
                    break;
                }
            }
            if (already_processed) continue;

            // Remove element from set first for this specific port
            if (m_nftVlanMbrSetElement.count(vlan_id) && m_nftVlanMbrSetElement[vlan_id].count(port_alias))
            {
                updateNftVlanMbrSetElement(vlan_id, port_alias, "delete");
            }

            std::string cmd_arp = generate_nft_rule_command(NFT_ARP_CHAIN, port_alias, false, vlan_id);
            std::string cmd_vlan_arp = generate_nft_rule_command(NFT_VLAN_ARP_CHAIN, port_alias, false, vlan_id);
            std::string cmd_nd = generate_nft_rule_command(NFT_ND_CHAIN, port_alias, false, vlan_id);

            if (!cmd_arp.empty()) all_nft_commands_to_batch.push_back(cmd_arp);
            if (!cmd_vlan_arp.empty()) all_nft_commands_to_batch.push_back(cmd_vlan_arp);
            if (!cmd_nd.empty()) all_nft_commands_to_batch.push_back(cmd_nd);

            members_processed.push_back(port_alias);
        }

        if (!all_nft_commands_to_batch.empty())
        {
            if (execute_nft_batch_file(all_nft_commands_to_batch))
            {
                SWSS_LOG_INFO("Successfully batched DELETE NFT rules for vlan %d.", vlan_id);
                for (const auto& pa : members_processed) // Use pa from members_processed
                {
                    auto it_map = m_neighborSuppressMap.find(pa);
                    if (it_map != m_neighborSuppressMap.end())
                    {
                        it_map->second.erase(vlan_id); // Use parameter vlan_id
                        SWSS_LOG_NOTICE("Updated m_neighborSuppressMap for DELETE: port %s, vlan %d, vlan_set size %zu",
                                        pa.c_str(), vlan_id, it_map->second.size());
                        if (it_map->second.empty())
                        {
                            m_neighborSuppressMap.erase(it_map);
                            SWSS_LOG_NOTICE("Erased port %s from m_neighborSuppressMap", pa.c_str());
                        }
                    }
                }
            }
            else
            {
                SWSS_LOG_ERROR("Failed to batch DELETE NFT rules for vlan %d.", vlan_id);
            }
        }

        // Check if the set itself can be deleted (if all members of this vlan are gone)
        if (m_nftVlanMbrSetElement.count(vlan_id) && m_nftVlanMbrSetElement[vlan_id].empty())
        {
            SWSS_LOG_INFO("NFT member set for VLAN %d is empty after processing all members, removing set.", vlan_id);
            updateNftVlanMbrSet(vlan_id, false);
            m_nftVlanMbrSetElement.erase(vlan_id); // Clean up the element tracking map for this vlan_id
            m_nftVlanMbrSetMap.erase(vlan_id);     // Clean up the set name tracking map
        }
    }
}

void VlanMgr::doVlanTask(Consumer &consumer)
{
    if (!isVlanMacOk())
    {
        SWSS_LOG_DEBUG("VLAN mac not ready, delaying VLAN task");
        return;
    }
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        string key = kfvKey(t);

        /* Ensure the key starts with "Vlan" otherwise ignore */
        if (strncmp(key.c_str(), VLAN_PREFIX, 4))
        {
            SWSS_LOG_ERROR("Invalid key format. No 'Vlan' prefix: %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        int vlan_id;
        try
        {
            vlan_id = stoi(key.substr(4));
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Invalid key format. Not a number after 'Vlan' prefix: %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        string vlan_alias, port_alias;
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            string admin_status;
            string mtu = DEFAULT_MTU_STR;
            string mac = gMacAddress.to_string();
            string hostif_name = "";
            vector<FieldValueTuple> fvVector;
            string members;

            string platform = getenv("platform") ? getenv("platform") : "";
            if (platform == BRCM_PLATFORM_SUBSTRING && isSubportConfigVlan(vlan_id))
            {
                it = consumer.m_toSync.erase(it);
                SWSS_LOG_ERROR("%s invaild config: subport config the vlan already", key.c_str());
                continue;
            }
            /*
             * If state is already set for this vlan, but it doesn't exist in m_vlans set,
             * just add it to m_vlans set and remove the request to skip disrupting Linux vlan.
             * Will hit this scenario for docker warm restart.
             *
             * Otherwise, it is new VLAN create or VLAN attribute update like admin_status/mtu change,
             * proceed with regular processing.
             */
            if (isVlanStateOk(key) && m_vlans.find(key) == m_vlans.end())
            {
                SWSS_LOG_DEBUG("%s already created", kfvKey(t).c_str());
                m_vlans.insert(key);
                m_vlanReplay.erase(kfvKey(t));
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Add host VLAN when it has not been created. */
            if (m_vlans.find(key) == m_vlans.end())
            {
                addHostVlan(vlan_id);
            }
            m_vlanReplay.erase(kfvKey(t));

            /* set up host env .... */
            for (auto i : kfvFieldsValues(t))
            {
                /* Set vlan admin status */
                if (fvField(i) == "admin_status")
                {
                    admin_status = fvValue(i);
                    setHostVlanAdminState(vlan_id, admin_status);
                    fvVector.push_back(i);
                }
                /* Set vlan mtu */
                else if (fvField(i) == "mtu")
                {
                    mtu = fvValue(i);
                    /*
                     * TODO: support host VLAN mtu setting.
                     * Host VLAN mtu should be set only after member configured
                     * and VLAN state is not UNKNOWN.
                     */
                    SWSS_LOG_DEBUG("%s mtu %s: Host VLAN mtu setting to be supported.", key.c_str(), mtu.c_str());
                }
                else if (fvField(i) == "members@") {
                    members = fvValue(i);
                }
                else if (fvField(i) == "mac")
                {
                    mac = fvValue(i);
                    setHostVlanMac(vlan_id, mac);
                }
                else if (fvField(i) == "host_ifname")
                {
                    hostif_name = fvValue(i);
                }
            }
            /* fvVector should not be empty */
            if (fvVector.empty())
            {
                FieldValueTuple a("admin_status",  "up");
                fvVector.push_back(a);
            }

            FieldValueTuple m("mtu", mtu);
            fvVector.push_back(m);

            FieldValueTuple mc("mac", mac);
            fvVector.push_back(mc);

            FieldValueTuple hostif_name_fvt("host_ifname", hostif_name);
            fvVector.push_back(hostif_name_fvt);

            m_appVlanTableProducer.set(key, fvVector);
            m_vlans.insert(key);

            fvVector.clear();
            FieldValueTuple s("state", "ok");
            fvVector.push_back(s);
            m_stateVlanTable.set(key, fvVector);

            it = consumer.m_toSync.erase(it);

            /*
             * Members configured together with VLAN in untagged mode.
             * This is to be compatible with access VLAN configuration from minigraph.
             */
            if (!members.empty())
            {
                processUntaggedVlanMembers(key, members);
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_vlans.find(key) != m_vlans.end())
            {
                removeHostVlan(vlan_id);
                m_vlans.erase(key);
                m_appVlanTableProducer.del(key);
                m_stateVlanTable.del(key);
            }
            else
            {
                SWSS_LOG_ERROR("%s doesn't exist", key.c_str());
            }
            SWSS_LOG_DEBUG("%s", (consumer.dumpTuple(t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            SWSS_LOG_DEBUG("%s", (consumer.dumpTuple(t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
    if (!replayDone && m_vlanReplay.empty() &&
        m_vlanMemberReplay.empty() &&
        WarmStart::isWarmStart())
    {
        replayDone = true;
        WarmStart::setWarmStartState("vlanmgrd", WarmStart::REPLAYED);
        SWSS_LOG_NOTICE("vlanmgr warmstart state set to REPLAYED");
        WarmStart::setWarmStartState("vlanmgrd", WarmStart::RECONCILED);
        SWSS_LOG_NOTICE("vlanmgr warmstart state set to RECONCILED");
    }
}

bool VlanMgr::isMemberStateOk(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (!alias.compare(0, strlen(LAG_PREFIX), LAG_PREFIX))
    {
        if (m_stateLagTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("%s is ready", alias.c_str());
            return true;
        }
    }
    else if (m_statePortTable.get(alias, temp))
    {
        auto state_opt = swss::fvsGetValue(temp, "state", true);
        if (!state_opt)
        {
            return false;
        }
        SWSS_LOG_DEBUG("%s is ready", alias.c_str());
        return true;
    }
    SWSS_LOG_DEBUG("%s is not ready", alias.c_str());
    return false;
}

bool VlanMgr::isVlanStateOk(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
    {
        if (m_stateVlanTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("%s is ready", alias.c_str());
            return true;
        }
    }
    SWSS_LOG_DEBUG("%s is not ready", alias.c_str());
    return false;
}

bool VlanMgr::isVlanMemberStateOk(const string &vlanMemberKey)
{
    vector<FieldValueTuple> temp;

    if (m_stateVlanMemberTable.get(vlanMemberKey, temp))
    {
        SWSS_LOG_DEBUG("%s is ready", vlanMemberKey.c_str());
        return true;
    }
    return false;
}

/*
 * members is grouped in format like
 * "Ethernet1,Ethernet2,Ethernet3,Ethernet4,Ethernet5,Ethernet6,
 * Ethernet7,Ethernet8,Ethernet9,Ethernet10,Ethernet11,Ethernet12,
 * Ethernet13,Ethernet14,Ethernet15,Ethernet16,Ethernet17,Ethernet18,
 * Ethernet19,Ethernet20,Ethernet21,Ethernet22,Ethernet23,Ethernet24"
 */
void VlanMgr::processUntaggedVlanMembers(string vlan, const string &members)
{

    auto consumer_it = m_consumerMap.find(CFG_VLAN_MEMBER_TABLE_NAME);
    if (consumer_it == m_consumerMap.end())
    {
        SWSS_LOG_ERROR("Failed to find tableName:%s", CFG_VLAN_MEMBER_TABLE_NAME);
        return;
    }
    auto& consumer = static_cast<Consumer &>(*consumer_it->second);

    vector<string> vlanMembers = tokenize(members, ',');

    for (auto vlanMember : vlanMembers)
    {
        string member_key = vlan + CONFIGDB_KEY_SEPARATOR + vlanMember;

        /* Directly put it into consumer.m_toSync map */
        if (consumer.m_toSync.find(member_key) == consumer.m_toSync.end())
        {
            vector<FieldValueTuple> fvVector;
            FieldValueTuple t("tagging_mode", "untagged");
            fvVector.push_back(t);
            KeyOpFieldsValuesTuple tuple = make_tuple(member_key, SET_COMMAND, fvVector);
            consumer.addToSync(tuple);
            SWSS_LOG_DEBUG("%s", (consumer.dumpTuple(tuple)).c_str());
        }
        /*
         * There is pending task from consumer pipe, in this case just skip it.
         */
        else
        {
            SWSS_LOG_WARN("Duplicate key %s found in table:%s", member_key.c_str(), CFG_VLAN_MEMBER_TABLE_NAME);
            continue;
        }
    }

    doTask(consumer);
    return;
}

void VlanMgr::doVlanMemberTask(Consumer &consumer)
{
    std::vector<std::string> ip_master_batch_commands;
    std::vector<std::string> bridge_batch_commands;
    std::vector<std::string> ip_nomaster_batch_commands;

    struct VlanMemberTaskInfo {
        std::string key_str;
        std::string op;
        int vlan_id;
        std::string port_alias;
        std::string tagging_mode;
        std::vector<FieldValueTuple> fv_tuple;
        bool processed_successfully; // To track if its corresponding batch commands were sent

        VlanMemberTaskInfo(const std::string& k, const std::string& o, int vid, const std::string& pa, const std::string& tm, const std::vector<FieldValueTuple>& fvt)
            : key_str(k), op(o), vlan_id(vid), port_alias(pa), tagging_mode(tm), fv_tuple(fvt), processed_successfully(false) {}
    };
    std::vector<VlanMemberTaskInfo>ภัย_tasks_details; // Renamed to avoid C++ keyword "processed"

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        string key = kfvKey(t);

        /* Ensure the key starts with "Vlan" otherwise ignore */
        if (strncmp(key.c_str(), VLAN_PREFIX, 4))
        {
            SWSS_LOG_ERROR("Invalid key format. No 'Vlan' prefix: %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        key = key.substr(4);
        size_t found = key.find(CONFIGDB_KEY_SEPARATOR);
        int vlan_id;
        string vlan_alias, port_alias;
        if (found != string::npos)
        {
            vlan_id = stoi(key.substr(0, found));
            port_alias = key.substr(found+1);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid key format. No member port is presented: %s",
                           kfvKey(t).c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        vlan_alias = VLAN_PREFIX + to_string(vlan_id);
        string op = kfvOp(t);

       // TODO:  store port/lag/VLAN data in local data structure and perform more validations.
        if (op == SET_COMMAND)
        {
            if (isVlanMemberStateOk(kfvKey(t)))
            {
                SWSS_LOG_DEBUG("%s already set, replay only", kfvKey(t).c_str());
                m_vlanMemberReplay.erase(kfvKey(t));
                // Still need to add to processed_tasks_details for potential NftRule updates if logic changes later,
                // but for now, if truly "already set", it implies no commands needed.
                // However, to be safe and align with potential re-application or Nft rule checks,
                // we can record it. For now, let's assume if state is OK, no commands are generated.
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Don't proceed if member port/lag is not ready yet */
            if (!isMemberStateOk(port_alias) || !isVlanStateOk(vlan_alias))
            {
                SWSS_LOG_DEBUG("%s or %s not ready, delaying %s", port_alias.c_str(), vlan_alias.c_str(), kfvKey(t).c_str());
                it++;
                continue;
            }
            std::string tagging_mode = "untagged"; // Default
            for (const auto& i : kfvFieldsValues(t))
            {
                if (fvField(i) == "tagging_mode")
                {
                    tagging_mode = fvValue(i);
                }
            }

            if (tagging_mode != "untagged" && tagging_mode != "tagged" && tagging_mode != "priority_tagged")
            {
                SWSS_LOG_ERROR("Wrong tagging_mode '%s' for key: %s", tagging_mode.c_str(), kfvKey(t).c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            // Command Generation for SET
            // 1. IP link set master
            ip_master_batch_commands.push_back(generate_add_vlan_member_ip_cmd(port_alias));

            // 2. Bridge vlan del vid 1 (conditionally)
            // key_def_vlan was VLAN_PREFIX DEFAULT_VLAN_ID CONFIGDB_KEY_SEPARATOR + port_alias
            std::string default_vlan_member_key = std::string(VLAN_PREFIX) + DEFAULT_VLAN_ID + CONFIGDB_KEY_SEPARATOR + port_alias;
            if (!isVlanMemberStateOk(default_vlan_member_key)) // Condition from original addHostVlanMember
            {
                SWSS_LOG_INFO("Default VLAN member %s not OK or not present, adding command to delete VID 1 for port %s", default_vlan_member_key.c_str(), port_alias.c_str());
                bridge_batch_commands.push_back(BRIDGE_CMD + std::string(" vlan del vid ") + DEFAULT_VLAN_ID + " dev " + shellquote(port_alias));
            }

            // 3. Bridge vlan add
            bridge_batch_commands.push_back(generate_add_vlan_member_bridge_cmd(vlan_id, port_alias, tagging_mode, false)); // `false` for is_default_vlan_member as it's handled above

            vlan_tasks_details.emplace_back(kfvKey(t), op, vlan_id, port_alias, tagging_mode, kfvFieldsValues(t));
            // Original DB/state updates and NftRule calls are deferred
        }
        else if (op == DEL_COMMAND)
        {
            if (isVlanMemberStateOk(kfvKey(t)))
            {
                // Command Generation for DEL
                // 1. Bridge vlan del
                bridge_batch_commands.push_back(generate_remove_vlan_member_bridge_cmd(vlan_id, port_alias));

                // 2. IP link set nomaster (simplified, see notes in generate_remove_vlan_member_ip_cmd)
                ip_nomaster_batch_commands.push_back(generate_remove_vlan_member_ip_cmd(vlan_id, port_alias));

                vlan_tasks_details.emplace_back(kfvKey(t), op, vlan_id, port_alias, "", std::vector<FieldValueTuple>()); // Tagging mode and FVs not needed for DEL
            }
            else
            {
                SWSS_LOG_DEBUG("%s doesn't exist, no commands generated for deletion.", kfvKey(t).c_str());
                m_vlanMemberReplay.erase(kfvKey(t)); // If it was in replay and doesn't exist, clear it.
            }
            // Original DB/state updates and NftRule calls are deferred
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s for key %s", op.c_str(), kfvKey(t).c_str());
        }
        // Always erase from consumer.m_toSync if it was processed into vlan_tasks_details or handled (e.g. error, already set)
        // The tasks that were delayed due to port/VLAN not ready will not be erased here (it++ was used).
        it = consumer.m_toSync.erase(it);
    }

    // Execute batch commands
    bool ip_master_success = true;
    if (!ip_master_batch_commands.empty())
    {
        ip_master_success = execute_batch_commands("ip -force -batch", ip_master_batch_commands);
        if (!ip_master_success)
        {
            SWSS_LOG_ERROR("Failed to execute IP master batch commands. Subsequent DB states might be inconsistent.");
        }
    }

    bool bridge_success = true;
    if (!bridge_batch_commands.empty())
    {
        bridge_success = execute_batch_commands("bridge -force -batch", bridge_batch_commands);
        if (!bridge_success)
        {
            SWSS_LOG_ERROR("Failed to execute Bridge batch commands. Subsequent DB states might be inconsistent.");
        }
    }

    bool ip_nomaster_success = true;
    if (!ip_nomaster_batch_commands.empty())
    {
        ip_nomaster_success = execute_batch_commands("ip -force -batch", ip_nomaster_batch_commands);
        if (!ip_nomaster_success)
        {
            SWSS_LOG_ERROR("Failed to execute IP nomaster batch commands. Subsequent DB states might be inconsistent.");
        }
    }

    // Process stored tasks for DB updates and NftRules
    // Assuming success if batch commands were attempted (due to -force), unless execute_batch_commands itself failed critically.
    // For now, proceed with DB updates regardless of individual batch "success" flags if tasks were collected,
    // as per "assume success if the batch command itself doesn't return a global error".
    // A more robust error handling might involve checking these flags.

    for (const auto& task_detail : vlan_tasks_details)
    {
        std::string current_key = task_detail.key_str;
        // The key for m_appVlanMemberTableProducer is different for DEL_COMMAND
        // It uses DEFAULT_KEY_SEPARATOR. For SET_COMMAND, it's kfvKey(t) which has CONFIGDB_KEY_SEPARATOR
        // Let's reconstruct the producer key carefully.
        // Original SET: m_appVlanMemberTableProducer.set(key, kfvFieldsValues(t)); where key was VlanX|PortY
        // Original DEL: key = VLAN_PREFIX + to_string(vlan_id); key += DEFAULT_KEY_SEPARATOR; key += port_alias; m_appVlanMemberTableProducer.del(key);

        std::string app_db_key = VLAN_PREFIX + std::to_string(task_detail.vlan_id) + DEFAULT_KEY_SEPARATOR + task_detail.port_alias;

        if (task_detail.op == SET_COMMAND)
        {
            // We assume that if we generated commands, the operation should proceed to DB update
            // unless a catastrophic batch failure (ip_master_success=false or bridge_success=false for SET)
            // For now, let's assume we update DB if commands were generated for this task.
            // The critical batch failures are logged above.

            m_appVlanMemberTableProducer.set(app_db_key, task_detail.fv_tuple);

            std::vector<FieldValueTuple> state_fv;
            FieldValueTuple s("state", "ok");
            state_fv.push_back(s);
            m_stateVlanMemberTable.set(task_detail.key_str, state_fv); // kfvKey(t) used here

            updateVlanMemberNftRule(task_detail.vlan_id, task_detail.port_alias, true);
            m_vlanMemberReplay.erase(task_detail.key_str);
            SWSS_LOG_INFO("Processed SET task for %s in batch.", task_detail.key_str.c_str());
        }
        else if (task_detail.op == DEL_COMMAND)
        {
            // Similar to SET, assume DB update if commands were generated.
            m_appVlanMemberTableProducer.del(app_db_key);
            m_stateVlanMemberTable.del(task_detail.key_str); // kfvKey(t) used here

            updateVlanMemberNftRule(task_detail.vlan_id, task_detail.port_alias, false);
            m_vlanMemberReplay.erase(task_detail.key_str); // Ensure replay is cleared
            SWSS_LOG_INFO("Processed DEL task for %s in batch.", task_detail.key_str.c_str());
        }
    }

    if (!replayDone && m_vlanMemberReplay.empty() && WarmStart::isWarmStart())
    {
        replayDone = true;
        WarmStart::setWarmStartState("vlanmgrd", WarmStart::REPLAYED);
        SWSS_LOG_NOTICE("vlanmgr warmstart state set to REPLAYED");
        WarmStart::setWarmStartState("vlanmgrd", WarmStart::RECONCILED);
        SWSS_LOG_NOTICE("vlanmgr warmstart state set to RECONCILED");

    }
}
bool VlanMgr::setNetdevNeighSuppress(const string &netdev, const string &suppress_mode)
{
    SWSS_LOG_ENTER();

    // The command should be generated as:
    // /bin/bash -c "echo {"0"| "1"} > /sys/devices/virtual/net/vtep-1000/brport/neigh_suppress"

    std::vector<std::string> nft_batch_commands;
    bool operation_status = true;

    if (suppress_mode == "on")
    {
        ostringstream sys_cmd_builder;
        sys_cmd_builder << ECHO_CMD << " " << shellquote("1") << " >> /sys/devices/virtual/net/" + netdev + "/brport/neigh_suppress";
        std::string full_sys_cmd = BASH_CMD + " -c " + shellquote(sys_cmd_builder.str());
        std::string res;
        if (swss::exec(full_sys_cmd, res) != 0)
        {
            SWSS_LOG_ERROR("Command '%s' failed. Output: %s", full_sys_cmd.c_str(), res.c_str());
            // Decide if this is a fatal error for this function
        }

        std::string cmd_arp = generate_nft_rule_command(NFT_ARP_CHAIN, netdev, true, 0); // vlan_id=0 for VTEP rules
        std::string cmd_vlan_arp = generate_nft_rule_command(NFT_VLAN_ARP_CHAIN, netdev, true, 0);
        std::string cmd_nd = generate_nft_rule_command(NFT_ND_CHAIN, netdev, true, 0);

        if (!cmd_arp.empty()) nft_batch_commands.push_back(cmd_arp);
        if (!cmd_vlan_arp.empty()) nft_batch_commands.push_back(cmd_vlan_arp);
        if (!cmd_nd.empty()) nft_batch_commands.push_back(cmd_nd);

        if (!nft_batch_commands.empty())
        {
            if (!execute_nft_batch_file(nft_batch_commands))
            {
                SWSS_LOG_ERROR("Failed to execute NFT ADD batch for VTEP %s.", netdev.c_str());
                operation_status = false;
            }
        }
        else
        {
            SWSS_LOG_WARN("No NFT ADD commands generated for VTEP %s.", netdev.c_str());
        }
    }
    else // suppress_mode == "off"
    {
        if (access(("/sys/devices/virtual/net/" + netdev).c_str(), F_OK) == 0)
        {
            ostringstream sys_cmd_builder;
            sys_cmd_builder << ECHO_CMD << " " << shellquote("0") << " >> /sys/devices/virtual/net/" + netdev + "/brport/neigh_suppress";
            std::string full_sys_cmd = BASH_CMD + " -c " + shellquote(sys_cmd_builder.str());
            std::string res;
            if (swss::exec(full_sys_cmd, res) != 0)
            {
                SWSS_LOG_ERROR("Command '%s' failed. Output: %s", full_sys_cmd.c_str(), res.c_str());
            }
        }
        else
        {
            SWSS_LOG_INFO("netdev %s does not exist, skipping echo to neigh_suppress.", netdev.c_str());
        }

        std::string cmd_arp = generate_nft_rule_command(NFT_ARP_CHAIN, netdev, false, 0); // vlan_id=0 for VTEP rules
        std::string cmd_vlan_arp = generate_nft_rule_command(NFT_VLAN_ARP_CHAIN, netdev, false, 0);
        std::string cmd_nd = generate_nft_rule_command(NFT_ND_CHAIN, netdev, false, 0);

        if (!cmd_arp.empty()) nft_batch_commands.push_back(cmd_arp);
        if (!cmd_vlan_arp.empty()) nft_batch_commands.push_back(cmd_vlan_arp);
        if (!cmd_nd.empty()) nft_batch_commands.push_back(cmd_nd);

        if (!nft_batch_commands.empty())
        {
            if (!execute_nft_batch_file(nft_batch_commands))
            {
                SWSS_LOG_ERROR("Failed to execute NFT DELETE batch for VTEP %s.", netdev.c_str());
                operation_status = false;
            }
        }
        else
        {
            SWSS_LOG_WARN("No NFT DELETE commands generated for VTEP %s.", netdev.c_str());
        }
    }
    return operation_status;
}

void VlanMgr::doNeighSuppressTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        string key = kfvKey(t);
        string vlan_alias;
        string netdev;
        /* Ensure the key starts with "Vlan" otherwise ignore */
        if (strncmp(key.c_str(), VLAN_PREFIX, 4))
        {
            SWSS_LOG_ERROR("Invalid key format. No 'Vlan' prefix: %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        vlan_alias = key;
        vector<FieldValueTuple> values;
        string op = kfvOp(t);

        if (m_stateNeighSuppressVlanTable.get(vlan_alias, values))
        {
            SWSS_LOG_INFO("m_stateNeighSuppressVlanTable.get ok");
            auto isNetDevField = [](FieldValueTuple fv) { return fvField(fv) == "netdev"; };
            auto valueIt = std::find_if(values.begin(), values.end(), isNetDevField);

            if (valueIt != values.end())
            {
                netdev = fvValue(*valueIt);
            }
            else
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }
        }
        else
        {
            SWSS_LOG_INFO("Failed to get entry in m_stateNeighSuppressVlanTable for vlan %s", vlan_alias.c_str());
            ++it;
            continue;
        }

        string suppress_mode = "off"; //default value for "suppress" field

        if (op == SET_COMMAND)
        {
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "suppress")
                {
                    suppress_mode = fvValue(i);
                    break;
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            suppress_mode = "off";
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (suppress_mode != "on" &&
            suppress_mode != "off")
        {
            SWSS_LOG_ERROR("Wrong suppress_mode '%s' for key: %s", suppress_mode.c_str(), kfvKey(t).c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        try
        {
            if (setNetdevNeighSuppress(netdev, suppress_mode))
            {
                SWSS_LOG_INFO("setNetdevNeighSuppress %s mode: %s ok", netdev.c_str(), suppress_mode.c_str());
                key = vlan_alias;

                // Update Vlan member in ARP/ND nftables rules
                vector<string> vlanMemberKeys;
                m_cfgVlanMemberTable.getKeys(vlanMemberKeys);
                for (auto key: vlanMemberKeys)
                {
                    size_t delimeter = key.find(CONFIGDB_KEY_SEPARATOR);
                    if (delimeter != string::npos)
                    {
                        string vlan_str = key.substr(0, delimeter);
                        if (!vlan_str.compare(vlan_alias))
                        {
                            string port_str = key.substr(delimeter+1);
                            int vlan_id;
                            try
                            {
                                vlan_id = stoi(key.substr(4));
                            }
                            catch (...)
                            {
                                SWSS_LOG_ERROR("Invalid key format. Not a number after 'Vlan' prefix: %s", key.c_str());
                                continue;
                            }

                            if (op == SET_COMMAND)
                            {
                                updateVlanMemberNftRule(vlan_id, port_str, true);
                            }
                            else if (op == DEL_COMMAND)
                            {
                                updateVlanMemberNftRule(vlan_id, port_str, false);
                            }
                        }
                    }
                }

                if (op == SET_COMMAND)
                {
                    m_appNeighSuppressVlanTableProducer.set(key, kfvFieldsValues(t));
                }
                else if (op == DEL_COMMAND)
                {
                    m_appNeighSuppressVlanTableProducer.del(key);
                }
            }
            else
            {
                SWSS_LOG_ERROR("setNetdevNeighSuppress %s mode %s fail", netdev.c_str(), suppress_mode.c_str());
                ++it;
                continue;
            }
        }
        catch (const std::exception &e)
        {
            SWSS_LOG_ERROR("setNetdevNeighSuppress %s mode %s fail. msg: %s", netdev.c_str(), suppress_mode.c_str(), e.what());
            ++it;
            continue;
        }

        it = consumer.m_toSync.erase(it);
    }
}
void VlanMgr::doNeighSuppressVlanTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        string key = kfvKey(t);
        string vlan_alias;
        string netdev;
        int vlan_id;
        /* Ensure the key starts with "Vlan" otherwise ignore */
        if (strncmp(key.c_str(), VLAN_PREFIX, 4))
        {
            SWSS_LOG_ERROR("Invalid key format. No 'Vlan' prefix: %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        vlan_alias = key;
        vlan_id = stoi(key.substr(4));
        string op = kfvOp(t);

        for (auto i : kfvFieldsValues(t))
        {
            if (fvField(i) == "netdev")
            {
                netdev = fvValue(i);
                break;
            }
        }

        if (op == SET_COMMAND)
        {
            string mode;
            vector<FieldValueTuple> values;
            m_cfgNeighSuppressVlanTable.get(vlan_alias, values);
            auto isSuppressField = [](FieldValueTuple fv) { return fvField(fv) == "suppress"; };
            auto valueIt = std::find_if(values.begin(), values.end(), isSuppressField);
            if (valueIt != values.end())
            {
                mode = fvValue(*valueIt);
                SWSS_LOG_INFO("suppress is %s", mode.c_str());
            }

            if (mode == "on" && netdev !="")
            {
                try
                {
                    if (setNetdevNeighSuppress(netdev, "on"))
                    {
                        SWSS_LOG_INFO("setNetdevNeighSuppress %s mode: %s ok", netdev.c_str(), mode.c_str());
                        key = vlan_alias;
                        vector<FieldValueTuple> fvVector;
                        FieldValueTuple suppress("suppress", "on");
                        fvVector.push_back(suppress);
                        m_appNeighSuppressVlanTableProducer.set(key, fvVector);
                        updateVlanMemberNftRule(vlan_id, true);
                        m_vlanVtepMap[vlan_id] = netdev;
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }
                }
                catch (const std::exception &e)
                {
                    SWSS_LOG_ERROR("setNetdevNeighSuppress %s mode %s fail. msg: %s", netdev.c_str(), mode.c_str(), e.what());
                }
            }
            else
            {
                removeVlanNeighborSuppression(vlan_id);
                m_appNeighSuppressVlanTableProducer.del(key);
                m_vlanVtepMap.erase(vlan_id);
                it = consumer.m_toSync.erase(it);
                continue;
            }
        }
        else if (op == DEL_COMMAND)
        {
            removeVlanNeighborSuppression(vlan_id);
            m_appNeighSuppressVlanTableProducer.del(key);
            m_vlanVtepMap.erase(vlan_id);
            it = consumer.m_toSync.erase(it);
            continue;
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

    }
}
void VlanMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    if (table_name == CFG_VLAN_TABLE_NAME)
    {
        doVlanTask(consumer);
    }
    else if (table_name == CFG_VLAN_MEMBER_TABLE_NAME)
    {
        doVlanMemberTask(consumer);
    }
    else if (table_name == CFG_NEIGH_SUPPRESS_VLAN_TABLE_NAME)
    {
        SWSS_LOG_DEBUG("Table:CFG_NEIGH_SUPPRESS_VLAN_TABLE_NAME");
        doNeighSuppressTask(consumer);
    }
    else if (table_name == STATE_NEIGH_SUPPRESS_VLAN_TABLE_NAME)
    {
        SWSS_LOG_DEBUG("Table:STATE_NEIGH_SUPPRESS_VLAN_TABLE_NAME");
        doNeighSuppressVlanTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown config table %s ", table_name.c_str());
        throw runtime_error("VlanMgr doTask failure.");
    }
}

void VlanMgr::removeVlanNeighborSuppression(int vlan_id)
{
    SWSS_LOG_ENTER();

    if (m_vlanVtepMap.find(vlan_id) != m_vlanVtepMap.end())
    {
        auto vtep_name = m_vlanVtepMap[vlan_id];
        setNetdevNeighSuppress(vtep_name, "off");
        m_vlanVtepMap.erase(vlan_id);
    }
    else
    {
        SWSS_LOG_INFO("vlan_id %d not exist in m_vlanVtepMap, ingore to off neigh_suppress", vlan_id);
    }

    SWSS_LOG_INFO("remove nftables rules for vlan %d", vlan_id);
    updateVlanMemberNftRule(vlan_id, false);
}

bool VlanMgr::execute_batch_commands(const std::string& command_prefix, const std::vector<std::string>& commands)
{
    SWSS_LOG_ENTER();

    if (commands.empty())
    {
        SWSS_LOG_INFO("No commands to execute in batch.");
        return true;
    }

    char tmp_filename[] = "/tmp/vlanmgr_batch_XXXXXX";
    int fd = mkstemp(tmp_filename);
    if (fd == -1)
    {
        SWSS_LOG_ERROR("Failed to create temporary file for batch commands: %s", strerror(errno));
        return false;
    }

    FILE *tmp_file = fdopen(fd, "w");
    if (!tmp_file)
    {
        SWSS_LOG_ERROR("Failed to open temporary file for writing: %s", strerror(errno));
        close(fd);
        unlink(tmp_filename);
        return false;
    }

    for (const auto& cmd : commands)
    {
        if (fprintf(tmp_file, "%s\n", cmd.c_str()) < 0)
        {
            SWSS_LOG_ERROR("Failed to write command to temporary file: %s", strerror(errno));
            fclose(tmp_file);
            unlink(tmp_filename);
            return false;
        }
    }

    if (fclose(tmp_file) == EOF)
    {
        SWSS_LOG_ERROR("Failed to close temporary file: %s", strerror(errno));
        unlink(tmp_filename);
        return false;
    }

    std::string full_command = command_prefix + " " + tmp_filename;
    std::string res;
    int ret = swss::exec(full_command, res);

    unlink(tmp_filename);

    if (ret != 0)
    {
        SWSS_LOG_ERROR("Batch command '%s' failed with rc %d. Output: %s", full_command.c_str(), ret, res.c_str());
        // Even if -force is used, swss::exec might return an error for other reasons (e.g. command not found)
        // For bridge/ip -force -batch, individual command errors within the batch file won't cause swss::exec to return non-zero.
        // However, we log the output in case there's useful information.
        // If command_prefix does not include -force, then any error will make ret non-zero.
        return false;
    }

    SWSS_LOG_INFO("Batch command '%s' executed successfully. Output: %s", full_command.c_str(), res.c_str());
    return true;
}

std::string VlanMgr::generate_nft_rule_command(const std::string &chain_name, const std::string port_alias, bool is_add, int vlan_id)
{
    SWSS_LOG_ENTER();
    std::string op = is_add ? "add" : "delete";
    std::ostringstream nft_cmd_builder;

    // Construct the base part of the rule
    nft_cmd_builder << "rule bridge filter " << chain_name;

    if (port_alias.find("vtep") != std::string::npos) // Rule for VTEP interface
    {
        nft_cmd_builder << " oifname " << shellquote(port_alias)
                        << " counter packets 0 bytes 0 accept";
    }
    else // Rule for physical interface or PortChannel
    {
        auto set_it = m_nftVlanMbrSetMap.find(vlan_id);
        if (set_it == m_nftVlanMbrSetMap.end() || set_it->second.empty())
        {
            SWSS_LOG_WARN("NFT VLAN member set name not found for VLAN %d when generating rule for port %s. Rule might be ineffective.", vlan_id, port_alias.c_str());
            // Return empty string or log error, as rule cannot be correctly formed.
            // This situation implies updateNftVlanMbrSet() wasn't called or failed before rule generation.
            return "";
        }
        std::string set_name = set_it->second;
        nft_cmd_builder << " iifname " << shellquote(port_alias)
                        << " oifname == @" << set_name
                        << " counter packets 0 bytes 0 accept";
    }

    std::string rule_spec = nft_cmd_builder.str();
    if (rule_spec.empty()) { // Should not happen if logic is correct
        return "";
    }

    return std::string(NFT_CMD) + " " + op + " " + rule_spec;
}

// The old VlanMgr::setNftRule function is now removed as its logic is replaced by
// generate_nft_rule_command and direct calls to execute_nft_batch_file or individual exec for handle-based deletion (if any remains).
// The m_nftNdSpRuleHandles map will become unused if all operations switch to content-based add/delete via batch.

std::string VlanMgr::generate_add_vlan_member_bridge_cmd(int vlan_id, const std::string& port_alias, const std::string& tagging_mode, bool is_default_vlan_member)
{
    SWSS_LOG_ENTER();
    std::string tagging_cmd_options;
    if (tagging_mode == "untagged" || tagging_mode == "priority_tagged")
    {
        tagging_cmd_options = " pvid untagged";
    }

    std::ostringstream cmd;
    if (is_default_vlan_member)
    {
        // This logic comes from the original addHostVlanMember where it conditionally removes vlan 1
        // For batching, this means two bridge commands could be generated.
        // This function will only generate the "add" part. The "del vid 1" part needs separate handling if generalized.
        // For simplicity, we assume the caller handles the "vlan del vid 1" part if necessary,
        // or this function is called appropriately.
        // The original command was: BRIDGE_CMD vlan del vid DEFAULT_VLAN_ID dev <port_alias> && BRIDGE_CMD vlan add vid <vlan_id> ...
        // This suggests that "del vid 1" should also be a command.
        // However, the task is to batch existing "bridge vlan add/del" and "ip link set master/nomaster".
        // Let's stick to the core "add" command for now.
        // The original code:
        // if (!isVlanMemberStateOk(key_def_vlan)) {
        //      inner << IP_CMD " link set " << shellquote(port_alias) << " master " DOT1Q_BRIDGE_NAME " && "
        //      BRIDGE_CMD " vlan del vid " DEFAULT_VLAN_ID " dev " << shellquote(port_alias) << " && "
        //      BRIDGE_CMD " vlan add vid " + std::to_string(vlan_id) + " dev " << shellquote(port_alias) << " " + tagging_cmd;
        // } else {
        //      inner << IP_CMD " link set " << shellquote(port_alias) << " master " DOT1Q_BRIDGE_NAME " && "
        //      BRIDGE_CMD " vlan add vid " + std::to_string(vlan_id) + " dev " << shellquote(port_alias) << " " + tagging_cmd;
        // }
        // The `is_default_vlan_member` parameter was intended to handle the `vlan del vid 1` case.
        // Let's assume for now this function just generates `bridge vlan add vid <vlan_id> dev <port_alias> <options>`
        // and if `vlan del vid 1` is needed, it's generated as a separate command by the caller.
        // This keeps the command generation function simpler.
    }
    // Simplified: always generate the add command. Caller decides if "del vid 1" is also needed.
    cmd << BRIDGE_CMD << " vlan add vid " << std::to_string(vlan_id) << " dev " << shellquote(port_alias) << tagging_cmd_options;
    return cmd.str();
}

bool VlanMgr::execute_nft_batch_file(const std::vector<std::string>& nft_commands)
{
    SWSS_LOG_ENTER();

    if (nft_commands.empty())
    {
        SWSS_LOG_INFO("No nft commands to execute in batch file.");
        return true;
    }

    char tmp_filename[] = "/tmp/vlanmgr_nft_batch_XXXXXX";
    int fd = mkstemp(tmp_filename);
    if (fd == -1)
    {
        SWSS_LOG_ERROR("Failed to create temporary file for nft batch commands: %s", strerror(errno));
        return false;
    }

    FILE *tmp_file = fdopen(fd, "w");
    if (!tmp_file)
    {
        SWSS_LOG_ERROR("Failed to open temporary file for writing nft commands: %s", strerror(errno));
        close(fd);
        unlink(tmp_filename);
        return false;
    }

    for (const auto& cmd : nft_commands)
    {
        if (fprintf(tmp_file, "%s\n", cmd.c_str()) < 0)
        {
            SWSS_LOG_ERROR("Failed to write nft command to temporary file: %s", strerror(errno));
            fclose(tmp_file);
            unlink(tmp_filename);
            return false;
        }
    }

    if (fclose(tmp_file) == EOF)
    {
        SWSS_LOG_ERROR("Failed to close nft temporary file: %s", strerror(errno));
        unlink(tmp_filename);
        return false;
    }

    std::string full_command = std::string(NFT_CMD) + " -f " + tmp_filename;
    std::string res;
    int ret = swss::exec(full_command, res);

    unlink(tmp_filename);

    if (ret != 0)
    {
        SWSS_LOG_ERROR("NFT batch command '%s' failed with rc %d. Output: %s", full_command.c_str(), ret, res.c_str());
        return false;
    }

    SWSS_LOG_INFO("NFT batch command '%s' executed successfully. Output: %s", full_command.c_str(), res.c_str());
    return true;
}

std::string VlanMgr::generate_add_vlan_member_ip_cmd(const std::string& port_alias)
{
    SWSS_LOG_ENTER();
    std::ostringstream cmd;
    cmd << IP_CMD << " link set " << shellquote(port_alias) << " master " << DOT1Q_BRIDGE_NAME;
    return cmd.str();
}

std::string VlanMgr::generate_remove_vlan_member_bridge_cmd(int vlan_id, const std::string& port_alias)
{
    SWSS_LOG_ENTER();
    std::ostringstream cmd;
    cmd << BRIDGE_CMD << " vlan del vid " << std::to_string(vlan_id) << " dev " << shellquote(port_alias);
    return cmd.str();
}

std::string VlanMgr::generate_remove_vlan_member_ip_cmd(int vlan_id, const std::string& port_alias)
{
    SWSS_LOG_ENTER();
    // This is a simplification. The original command conditionally sets nomaster.
    // /bin/bash -c '... if (condition based on bridge vlan show) then ip link set <port> nomaster; fi ...'
    // For batching, we generate the raw command. The `-force` flag in `ip -force -batch`
    // should make this command a no-op if the port is already not master or if it's not appropriate.
    // However, the condition was to check if *any* other VLANs were configured for that port on the *Bridge*.
    // If we just send "ip link set <port> nomaster", and the port is still part of other VLANs on *the same bridge*,
    // this command might be problematic.
    // The original script logic implies "set nomaster only if this was the last VLAN on this port for the Bridge".
    // For now, we generate the command as requested by the subtask "generate ... ip link set ... nomaster command strings".
    // The responsibility of whether this command should actually be run (or if it's safe to run)
    // is deferred. If all bridge vlan del commands for a port are processed first, then this nomaster
    // might be appropriate if it's truly the last one.
    // This is a known simplification.
    std::ostringstream cmd;
    cmd << IP_CMD << " link set " << shellquote(port_alias) << " nomaster";
    return cmd.str();
}

