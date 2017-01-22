/*
 * Copyright (C) 2014 - 2015 Xilinx, Inc.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * Use of the Software is limited solely to applications:
 * (a) running on a Xilinx device, or
 * (b) that interact with a Xilinx device through a bus or interconnect.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Except as contained in this notice, the name of the Xilinx shall not be used
 * in advertising or otherwise to promote the sale, use or other dealings in
 * this Software without prior written authorization from Xilinx.
 */

/*************************************************************************
 * PM slave structures definitions and code for handling states of slaves.
 ************************************************************************/

#include "pm_slave.h"
#include "pm_requirement.h"
#include "pm_defs.h"
#include "pm_common.h"
#include "pm_node.h"
#include "pm_sram.h"
#include "pm_usb.h"
#include "pm_periph.h"
#include "pm_pll.h"
#include "pm_power.h"
#include "lpd_slcr.h"
#include "pm_ddr.h"
#include "pm_clock.h"
#include <unistd.h>
#include "pm_gpp.h"

/**
 * PmGetMaxCapabilities()- Get maximum of all requested capabilities of slave
 * @slave   Slave whose maximum required capabilities should be determined
 *
 * @return  32bit value encoding the capabilities
 */
static u32 PmGetMaxCapabilities(const PmSlave* const slave)
{
	PmRequirement* req = slave->reqs;
	u32 maxCaps = 0U;

	while (NULL != req) {
		if (0U != (PM_MASTER_USING_SLAVE_MASK & req->info)) {
			maxCaps |= req->currReq;
		}
		req = req->nextMaster;
	}

	return maxCaps;
}

/**
 * PmCheckCapabilities() - Check whether the slave has state with specified
 *                         capabilities
 * @slave   Slave pointer whose capabilities/states should be checked
 * @cap     Check for these capabilities
 *
 * @return  Status wheter slave has a state with given capabilities
 *          - XST_SUCCESS if slave has state with given capabilities
 *          - XST_NO_FEATURE if slave does not have such state
 */
int PmCheckCapabilities(const PmSlave* const slave, const u32 capabilities)
{
	PmStateId i;
	int status = XST_NO_FEATURE;

	for (i = 0U; i < slave->slvFsm->statesCnt; i++) {
		/* Find the first state that contains all capabilities */
		if ((capabilities & slave->slvFsm->states[i]) == capabilities) {
			status = XST_SUCCESS;
			break;
		}
	}

	return status;
}

/**
 * PmSlaveHasWakeUpCap() - Check if the slave has a wake-up capability
 * @slv		Slave to be checked
 *
 * @return	XST_SUCCESS if the slave has the wake-up capability
 *		XST_NO_FEATURE if the slave doesn't have the wake-up capability
 */
int PmSlaveHasWakeUpCap(const PmSlave* const slv)
{
	int status;

	/* Check is the slave's pointer to the GIC Proxy wake initialized */
	if (NULL == slv->wake) {
		status = XST_NO_FEATURE;
		goto done;
	}

	/* Check whether the slave has a state with wake-up capability */
	status = PmCheckCapabilities(slv, PM_CAP_WAKEUP);

done:
	return status;
}

/**
 * PmSlavePrepareState() - Prepare for entering a state
 * @slv		Slave that would enter next state
 * @next	Next state the slave would enter
 *
 * @return	Status fo preparing for the transition (XST_SUCCESS or an error
 *		code)
 */
static int PmSlavePrepareState(PmSlave* const slv, const PmStateId next)
{
	int status = XST_SUCCESS;
	const PmStateId curr = slv->node.currState;

	/* If slave has power parent make sure the parent is in proper state */
	if (NULL != slv->node.parent) {

		if ((0U == (slv->slvFsm->states[curr] & PM_CAP_POWER)) &&
		    (0U != (slv->slvFsm->states[next] & PM_CAP_POWER))) {
			status = PmPowerRequestParent(&slv->node);
			if (XST_SUCCESS != status) {
				goto done;
			}
		}
	}

	/* Check if slave requires clocks in the next state */
	if (NULL != slv->node.clocks) {
		if ((0U == (slv->slvFsm->states[curr] & PM_CAP_CLOCK)) &&
		    (0U != (slv->slvFsm->states[next] & PM_CAP_CLOCK))) {
			status = PmClockRequest(&slv->node);
		}
	}

done:
	return status;
}

/**
 * PmSlaveClearAfterState() - Clean after exiting a state
 * @slv		Slave that exited the prev state
 * @prev	Previous state the slave was in
 */
static void PmSlaveClearAfterState(PmSlave* const slv, const PmStateId prev)
{
	const PmStateId curr = slv->node.currState;

	/* Check if slave doesn't use clocks in the new state */
	if (NULL != slv->node.clocks) {
		if ((0U != (slv->slvFsm->states[prev] & PM_CAP_CLOCK)) &&
		    (0U == (slv->slvFsm->states[curr] & PM_CAP_CLOCK))) {
			PmClockRelease(&slv->node);
		}
	}

	/* Check if slave doesn't need power in the new state */
	if (NULL != slv->node.parent) {
		if ((0U != (slv->slvFsm->states[prev] & PM_CAP_POWER)) &&
		    (0U == (slv->slvFsm->states[curr] & PM_CAP_POWER))) {
			PmPowerReleaseParent(&slv->node);
		}
	}

}

/**
 * PmSlaveChangeState() - Change state of a slave
 * @slave       Slave pointer whose state should be changed
 * @state       New state
 *
 * @return      XST_SUCCESS if transition was performed successfully.
 *              Error otherwise.
 */
static int PmSlaveChangeState(PmSlave* const slave, const PmStateId state)
{
	u32 t;
	int status;
	const PmSlaveFsm* fsm = slave->slvFsm;
	PmStateId oldState = slave->node.currState;

	/* Check what needs to be done prior to performing the transition */
	status = PmSlavePrepareState(slave, state);
	if (XST_SUCCESS != status) {
		goto done;
	}

	if (0U == fsm->transCnt) {
		/* Slave's FSM has no transitions when it has only one state */
		status = XST_SUCCESS;
	} else {
		/*
		 * Slave has transitions to change the state. Assume the failure
		 * and change status if state is changed correctly.
		 */
		status = XST_FAILURE;
	}

	for (t = 0U; t < fsm->transCnt; t++) {
		/* Find transition from current state to state to be set */
		if ((fsm->trans[t].fromState != slave->node.currState) ||
			(fsm->trans[t].toState != state)) {
			continue;
		}
		if (NULL != slave->slvFsm->enterState) {
			/* Execute transition action of slave's FSM */
			status = slave->slvFsm->enterState(slave, state);
		} else {
			status = XST_SUCCESS;
		}
		break;
	}

done:
	if ((oldState != state) && (XST_SUCCESS == status)) {
		PmNodeUpdateCurrState(&slave->node, state);
		PmSlaveClearAfterState(slave, oldState);
	}
#ifdef DEBUG_PM
	if (XST_SUCCESS == status) {
		PmDbg("%s %d->%d\r\n", PmStrNode(slave->node.nodeId), oldState,
		      slave->node.currState);
	} else {
		PmDbg("%s ERROR #%d\r\n", PmStrNode(slave->node.nodeId), status);
	}
#endif
	return status;
}

/**
 * PmGetStateWithCaps() - Get id of the state with provided capabilities
 * @slave       Slave whose states are searched
 * @caps        Capabilities the state must have
 * @state       Pointer to a PmStateId variable where the result is put if
 *              state is found
 *
 * @return      Status of the operation
 *              - XST_SUCCESS if state is found
 *              - XST_NO_FEATURE if state with required capabilities does not
 *                exist
 *
 * This function is to be called when state of a slave should be updated,
 * to find the slave's state with required capabilities.
 * Argument caps has included capabilities requested by all masters which
 * currently use the slave. Although these separate capabilities are validated
 * at the moment request is made, it could happen that there is no state that
 * has capabilities requested by all masters. This conflict has to be resolved
 * between the masters, so PM returns an error.
 */
static int PmGetStateWithCaps(const PmSlave* const slave, const u32 caps,
				  PmStateId* const state)
{
	PmStateId i;
	int status = XST_PM_CONFLICT;

	for (i = 0U; i < slave->slvFsm->statesCnt; i++) {
		/* Find the first state that contains all capabilities */
		if ((caps & slave->slvFsm->states[i]) == caps) {
			status = XST_SUCCESS;
			if (NULL != state) {
				*state = i;
			}
			break;
		}
	}

	return status;
}

/**
 * PmGetMinRequestedLatency() - Find minimum of all latency requirements
 * @slave       Slave whose min required latency should be found
 *
 * @return      Latency in microseconds
 */
static u32 PmGetMinRequestedLatency(const PmSlave* const slave)
{
	PmRequirement* req = slave->reqs;
	u32 minLatency = MAX_LATENCY;

	while (NULL != req) {
		if (0U != (PM_MASTER_SET_LATENCY_REQ & req->info)) {
			if (minLatency > req->latencyReq) {
				minLatency = req->latencyReq;
			}
		}
		req = req->nextMaster;
	}

	return minLatency;
}

/**
 * PmGetLatencyFromToState() - Get latency from given state to the highest state
 * @slave       Pointer to the slave whose states are in question
 * @state       State from which the latency is calculated
 *
 * @return      Return value for the found latency
 */
static u32 PmGetLatencyFromState(const PmSlave* const slave,
			  const PmStateId state)
{
	u32 i, latency = 0U;
	PmStateId highestState = slave->slvFsm->statesCnt - 1;

	for (i = 0U; i < slave->slvFsm->transCnt; i++) {
		if ((state == slave->slvFsm->trans[i].fromState) &&
		    (highestState == slave->slvFsm->trans[i].toState)) {
			latency = slave->slvFsm->trans[i].latency;
			break;
		}
	}

	return latency;
}

/**
 * PmConstrainStateByLatency() - Find a higher power state which satisfies
 *                               latency requirements
 * @slave       Slave whose state may be constrained
 * @state       Chosen state which does not satisfy latency requirements
 * @capsToSet   Capabilities that the state must have
 * @minLatency  Latency requirements to be satisfied
 *
 * @return      Status showing whether the higher power state is found or not.
 *              State may not be found if multiple masters have contradicting
 *              requirements, then XST_PM_CONFLICT is returned. Otherwise,
 *              function returns success.
 */
static int PmConstrainStateByLatency(const PmSlave* const slave,
				     PmStateId* const state,
				     const u32 capsToSet,
				     const u32 minLatency)
{
	int status = XST_PM_CONFLICT;
	PmStateId startState = *state;
	u32 wkupLat, i;

	for (i = startState; i < slave->slvFsm->statesCnt; i++) {
		if ((capsToSet & slave->slvFsm->states[i]) != capsToSet) {
			/* State candidate has no required capabilities */
			continue;
		}
		wkupLat = PmGetLatencyFromState(slave, i);
		if (wkupLat > minLatency) {
			/* State does not satisfy latency requirement */
			continue;
		}

		status = XST_SUCCESS;
		*state = i;
		break;
	}

	return status;
}

/**
 * PmUpdateSlave() - Update the slave's state according to the current
 *                   requirements from all masters
 * @slave       Slave whose state is about to be updated
 *
 * @return      Status of operation of updating slave's state.
 *
 * @note	A slave may not have state with zero capabilities. If that is
 * the case and no capabilities are requested, it is put in lowest power state
 * (state ID 0).
 * When non-zero capabilities are requested and a selected state which has the
 * requested capabilities doesn't satisfy the wake-up latency requirements, the
 * first higher power state which satisfies latency requirement and has the
 * requested capabilities is configured (in the worst case it's the highest
 * power state).
 */
int PmUpdateSlave(PmSlave* const slave)
{
	PmStateId state = 0U;
	int status = XST_SUCCESS;
	u32 wkupLat, minLat;
	u32 caps = PmGetMaxCapabilities(slave);

	if (0U != caps) {
		/* Find which state has the requested capabilities */
		status = PmGetStateWithCaps(slave, caps, &state);
		if (XST_SUCCESS != status) {
			goto done;
		}
	}

	minLat = PmGetMinRequestedLatency(slave);
	wkupLat = PmGetLatencyFromState(slave, state);
	if (wkupLat > minLat) {
		/* State does not satisfy latency requirement, find another */
		status = PmConstrainStateByLatency(slave, &state, caps, minLat);
		if (XST_SUCCESS != status) {
			goto done;
		}
		wkupLat = PmGetLatencyFromState(slave, state);
	}

	slave->node.latencyMarg = minLat - wkupLat;
	if (state != slave->node.currState) {
		status = PmSlaveChangeState(slave, state);
		if (XST_SUCCESS != status) {
			goto done;
		}
	} else {
		if (!HAS_CAPABILITIES(slave, state, PM_CAP_POWER)) {
			/* Notify power parent (changed latency requirement) */
			status = PmPowerUpdateLatencyReq(&slave->node);
		}
	}

done:
	return status;
}

/**
 * PmSlaveGetUsersMask() - Gets all masters' mask currently using the slave
 * @slave       Slave in question
 *
 * @return      Each master has unique ipiMask which identifies it (one hot
 *              encoding). Return value represents ORed masks of all masters
 *              which are currently using the slave.
 */
u32 PmSlaveGetUsersMask(const PmSlave* const slave)
{
	PmRequirement* req = slave->reqs;
	u32 usage = 0U;

	while (NULL != req) {
		if (0U != (PM_MASTER_USING_SLAVE_MASK & req->info)) {
			/* Found master which is using slave */
			usage |= req->master->ipiMask;
		}
		req = req->nextMaster;
	}

	return usage;
}

/**
 * PmSlaveGetUsageStatus() - get current usage status for a slave node
 * @slave      Slave node for which the usage status is requested
 * @master     Master that's requesting the current usage status
 *
 * @return  Usage status:
 *	    - 0: No master is currently using the node
 *	    - 1: Only requesting master is currently using the node
 *	    - 2: Only other masters (1 or more) are currently using the node
 *	    - 3: Both the current and at least one other master is currently
 *               using the node
 */
u32 PmSlaveGetUsageStatus(const PmSlave* const slave,
			  const PmMaster* const master)
{
	u32 usageStatus = 0;
	const PmRequirement* req = slave->reqs;

	while (NULL != req) {

		if (0U != (req->info & PM_MASTER_USING_SLAVE_MASK)) {
			/* This master is currently using this slave */
			if (master == req->master) {
				usageStatus |= PM_USAGE_CURRENT_MASTER;
			} else {
				usageStatus |= PM_USAGE_OTHER_MASTER;
			}
		}
		req = req->nextMaster;
	}

	return usageStatus;
}

/**
 * PmSlaveGetRequirements() - get current requirements for a slave node
 * @slave      Slave node for which the current requirements are requested
 * @master     Master that's making the request
 *
 * @return  Current requirements of the requesting master on the node
 */
u32 PmSlaveGetRequirements(const PmSlave* const slave,
			   const PmMaster* const master)
{
	u32 currReq = 0;
	PmRequirement* masterReq = PmRequirementGet(master, slave);

	if (NULL == masterReq) {
		/* This master has no access to this slave */
		goto done;
	}

	if (0U == (masterReq->info & PM_MASTER_USING_SLAVE_MASK)) {
		/* This master is currently not using this slave */
		goto done;
	}

	/* This master is currently using this slave */
	currReq = masterReq->currReq;

done:
	return currReq;
}

/**
 * PmSlaveVerifyRequest() - Check whether PM framework can grant the request
 * @slave       Slave node that is requested
 *
 * @return      XST_SUCCESS if the following condition is satisfied : (slave
 *              is shareable) OR (it is exclusively used AND no other master
 *              currently uses the slave)
 *              XST_PM_NODE_USED otherwise
 */
int PmSlaveVerifyRequest(const PmSlave* const slave)
{
	int status = XST_SUCCESS;
	u32 usage;

	/* If slave is shareable the request is ok */
	if (0U != (PM_SLAVE_FLAG_IS_SHAREABLE & slave->flags)) {
		goto done;
	}

	usage = PmSlaveGetUsersMask(slave);
	/* Slave is not shareable, if it is unused the request is ok */
	if (0U == usage) {
		goto done;
	}

	/* Slave request cannot be granted, node is non-shareable and used */
	status = XST_PM_NODE_USED;

done:
	return status;
}

/**
 * PmSlaveSetConfig() - Set the configuration for the slave
 * @slave       Slave to configure
 * @policy      Usage policy for the slave to configure
 * @perms       Permissions to use the slave (ORed IPI masks of permissible
 *              masters)
 * @return      XST_SUCCESS if configuration is set, XST_FAILURE otherwise
 *
 * @note        For each master whose IPI is encoded in the 'perms', the
 *              requirements structure is automatically allocated and added in
 *              master's/slave's lists of requirements.
 */
int PmSlaveSetConfig(PmSlave* const slave, const u32 policy, const u32 perms)
{
	int status = XST_SUCCESS;
	u32 masterIpiMasks = perms;
	u32 i, masterCnt;

	if (0U != (policy & PM_SLAVE_FLAG_IS_SHAREABLE)) {
		slave->flags |= PM_SLAVE_FLAG_IS_SHAREABLE;
	}

	/*
	 * Number of masters allowed to use the slave is equal to the number of
	 * set bits. By extracting one-hot mask we get IPI mask associated with
	 * the master, then by IPI mask we get the pointer to master.
	 */
	masterCnt = __builtin_popcount(perms);
	for (i = 0U; i < masterCnt; i++) {
		u32 ipiMask;
		PmMaster* master;

		ipiMask = 1U << __builtin_ctz(masterIpiMasks);
		master = PmGetMasterByIpiMask(ipiMask);
		if (NULL == master) {
			status = XST_FAILURE;
			goto done;
		}

		status = PmRequirementAdd(master, slave);
		if (XST_SUCCESS != status) {
			goto done;
		}

		/* Done with this master, clear the bit */
		masterIpiMasks &= ~ipiMask;
	}

done:
	return status;
}

/**
 * PmSlaveClearConfig() - Clear configuration of the slave node
 * @slaveNode	Slave node to clear
 */
static void PmSlaveClearConfig(PmNode* const slaveNode)
{
	PmSlave* const slave = (PmSlave*)slaveNode->derived;

	slave->reqs = NULL;
	slave->flags = 0U;
}

/**
 * PmSlaveGetWakeUpLatency() - Get wake-up latency of the slave node
 * @node	Slave node
 * @lat		Pointer to the location where the latency value should be stored
 *
 * @return	XST_SUCCESS if latency value is stored in *lat, XST_NO_FEATURE
 *		if the latency depends on power parent which has no method
 *		(getWakeUpLatency) to provide latency information
 */
static int PmSlaveGetWakeUpLatency(const PmNode* const node, u32* const lat)
{
	PmSlave* const slave = (PmSlave*)node->derived;
	PmNode* const powerNode = &node->parent->node;
	int status = XST_SUCCESS;
	u32 latency;

	*lat = PmGetLatencyFromState(slave, slave->node.currState);


	if (NULL == powerNode->class->getWakeUpLatency) {
		status = XST_NO_FEATURE;
		goto done;
	}

	status = powerNode->class->getWakeUpLatency(powerNode, &latency);
	if (XST_SUCCESS == status) {
		*lat += latency;
	}

done:
	return status;

}

 /**
 * PmSlaveForceDown() - Force down the slave node
 * @node	Slave node to force down
 *
 * @return	Status of performing force down operation
 */
static int PmSlaveForceDown(PmNode* const node)
{
	int status = XST_SUCCESS;
	PmSlave* const slave = (PmSlave*)node->derived;
	PmRequirement* req = slave->reqs;

	while (NULL != req) {
		if (0U != (PM_MASTER_USING_SLAVE_MASK & req->info)) {
			PmRequirementClear(req);
		}
		req = req->nextMaster;
	}
	if (0U != slave->node.currState) {
		status = PmSlaveChangeState(slave, 0U);
	}

	return status;
}

/* Collection of slave nodes */
static PmNode* pmNodeSlaveBucket[] = {
	&pmSlaveL2_g.slv.node,
	&pmSlaveOcm0_g.slv.node,
	&pmSlaveOcm1_g.slv.node,
	&pmSlaveOcm2_g.slv.node,
	&pmSlaveOcm3_g.slv.node,
	&pmSlaveTcm0A_g.slv.node,
	&pmSlaveTcm0B_g.slv.node,
	&pmSlaveTcm1A_g.slv.node,
	&pmSlaveTcm1B_g.slv.node,
	&pmSlaveUsb0_g.slv.node,
	&pmSlaveUsb1_g.slv.node,
	&pmSlaveTtc0_g.node,
	&pmSlaveTtc1_g.node,
	&pmSlaveTtc2_g.node,
	&pmSlaveTtc3_g.node,
	&pmSlaveSata_g.node,
	&pmSlaveApll_g.slv.node,
	&pmSlaveVpll_g.slv.node,
	&pmSlaveDpll_g.slv.node,
	&pmSlaveRpll_g.slv.node,
	&pmSlaveIOpll_g.slv.node,
	&pmSlaveGpuPP0_g.slv.node,
	&pmSlaveGpuPP1_g.slv.node,
	&pmSlaveUart0_g.node,
	&pmSlaveUart1_g.node,
	&pmSlaveSpi0_g.node,
	&pmSlaveSpi1_g.node,
	&pmSlaveI2C0_g.node,
	&pmSlaveI2C1_g.node,
	&pmSlaveSD0_g.node,
	&pmSlaveSD1_g.node,
	&pmSlaveCan0_g.node,
	&pmSlaveCan1_g.node,
	&pmSlaveEth0_g.node,
	&pmSlaveEth1_g.node,
	&pmSlaveEth2_g.node,
	&pmSlaveEth3_g.node,
	&pmSlaveAdma_g.node,
	&pmSlaveGdma_g.node,
	&pmSlaveDP_g.node,
	&pmSlaveNand_g.node,
	&pmSlaveQSpi_g.node,
	&pmSlaveGpio_g.node,
	&pmSlaveAFI_g.node,
	&pmSlaveDdr_g.node,
	&pmSlaveIpiApu_g.node,
	&pmSlaveIpiRpu0_g.node,
	&pmSlaveGpu_g.node,
	&pmSlavePcie_g.node,
	&pmSlavePcap_g.node,
	&pmSlaveRtc_g.node,
};

PmNodeClass pmNodeClassSlave_g = {
	DEFINE_NODE_BUCKET(pmNodeSlaveBucket),
	.id = NODE_CLASS_SLAVE,
	.clearConfig = PmSlaveClearConfig,
	.getWakeUpLatency = PmSlaveGetWakeUpLatency,
	.getPowerData = PmNodeGetPowerInfo,
	.forceDown = PmSlaveForceDown,
};
