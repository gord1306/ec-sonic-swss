#!/bin/bash

# test_vlan_batch_performance.sh
#
# This script is designed to test the performance of vlanmgrd when adding
# a large number of VLANs and then adding a large number of ports to each VLAN.
# It also cleans up the created VLANs and members.
#
# Usage:
#   1. Ensure this script is executable: `chmod +x test_vlan_batch_performance.sh`
#   2. Run the script: `./test_vlan_batch_performance.sh`
#   3. The script will output the time taken for configuration and cleanup.
#
# Notes:
#   - This script should be run in a SONiC environment where `config` commands are available.
#   - The default numbers are high and designed for performance testing.
#     Adjust NUM_VLANS and NUM_PORTS if needed for quicker tests or different scenarios.
#   - The script starts VLAN IDs from 2, as Vlan1 often exists by default.

# --- Configuration ---
NUM_VLANS=${1:-10}      # Number of VLANs to create (e.g., 4000 for full test, default 10 for quick test)
NUM_PORTS=${2:-4}       # Number of ports to add to each VLAN (e.g., 54 for full test, default 4 for quick test)
VLAN_START_ID=2
PORT_PREFIX="Ethernet"
TAGGING_MODE="tagged" # "tagged" or "untagged"

echo "Starting VLAN batch performance test..."
echo "Number of VLANs to create: $NUM_VLANS"
echo "Number of ports per VLAN: $NUM_PORTS"
echo "VLAN Start ID: $VLAN_START_ID"
echo "Port Prefix: $PORT_PREFIX"
echo "Tagging Mode: $TAGGING_MODE"
echo "-----------------------------------------------------"

# Generate port aliases
PORTS=()
for ((i=0; i<NUM_PORTS; i++)); do
    PORTS+=("${PORT_PREFIX}${i}")
done

# --- Function to log current time and message ---
log_time() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') - $1"
}

# --- Phase 1: Create VLANs and add members ---
log_time "Starting VLAN and Member configuration phase..."
start_time_config=$(date +%s)

# Create VLANs
log_time "Creating $NUM_VLANS VLANs..."
for ((i=0; i<NUM_VLANS; i++)); do
    vlan_id=$((VLAN_START_ID + i))
    echo "Creating Vlan${vlan_id}"
    config vlan add "${vlan_id}"
    if [ $? -ne 0 ]; then
        log_time "ERROR: Failed to create Vlan${vlan_id}. Exiting."
        exit 1
    fi
done
log_time "VLAN creation complete."

# Add members to each VLAN
log_time "Adding $NUM_PORTS ports to each of the $NUM_VLANS VLANs..."
for ((i=0; i<NUM_VLANS; i++)); do
    vlan_id=$((VLAN_START_ID + i))
    log_time "Processing Vlan${vlan_id}..."
    for port_alias in "${PORTS[@]}"; do
        # echo "Adding ${port_alias} to Vlan${vlan_id} as ${TAGGING_MODE}"
        config vlan member add -m "${TAGGING_MODE}" "${vlan_id}" "${port_alias}"
        if [ $? -ne 0 ]; then
            log_time "ERROR: Failed to add ${port_alias} to Vlan${vlan_id}. Continuing with next, but test may be invalid."
            # Decide if to exit or continue: for now, continue
        fi
    done
done
log_time "VLAN member addition complete."

end_time_config=$(date +%s)
total_time_config=$((end_time_config - start_time_config))
log_time "Configuration phase took $total_time_config seconds."
echo "-----------------------------------------------------"


# --- Phase 2: Clean up VLANs and members ---
log_time "Starting Cleanup phase..."
start_time_cleanup=$(date +%s)

# Remove members from each VLAN
log_time "Removing $NUM_PORTS ports from each of the $NUM_VLANS VLANs..."
for ((i=0; i<NUM_VLANS; i++)); do
    vlan_id=$((VLAN_START_ID + i))
    log_time "Cleaning Vlan${vlan_id} members..."
    for port_alias in "${PORTS[@]}"; do
        # echo "Removing ${port_alias} from Vlan${vlan_id}"
        config vlan member del "${vlan_id}" "${port_alias}"
        if [ $? -ne 0 ]; then
            log_time "WARN: Failed to remove ${port_alias} from Vlan${vlan_id}. Manual cleanup might be needed."
        fi
    done
done
log_time "VLAN member removal complete."

# Remove VLANs
log_time "Removing $NUM_VLANS VLANs..."
for ((i=0; i<NUM_VLANS; i++)); do
    vlan_id=$((VLAN_START_ID + i))
    echo "Deleting Vlan${vlan_id}"
    config vlan del "${vlan_id}"
    if [ $? -ne 0 ]; then
        log_time "WARN: Failed to delete Vlan${vlan_id}. Manual cleanup might be needed."
    fi
done
log_time "VLAN deletion complete."

end_time_cleanup=$(date +%s)
total_time_cleanup=$((end_time_cleanup - start_time_cleanup))
log_time "Cleanup phase took $total_time_cleanup seconds."
echo "-----------------------------------------------------"

log_time "VLAN batch performance test finished."
echo "Total configuration time: $total_time_config seconds."
echo "Total cleanup time: $total_time_cleanup seconds."

exit 0
```
