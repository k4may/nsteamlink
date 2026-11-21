.PHONY: switch switch-diag switch-dualsense switch-deploy

# Build the runnable artifact, not just the ELF linker target.
switch:
	NSL_BUILD_TARGET=nsteamlink_nro ./scripts/build-switch.sh -DNSL_DEFAULT_DUALSENSE=OFF

switch-diag:
	NSL_BUILD_TARGET=nsteamlink_nro ./scripts/build-switch.sh -DNSL_DIAGNOSTICS=ON -DNSL_DEFAULT_DUALSENSE=OFF

switch-dualsense:
	NSL_BUILD_TARGET=nsteamlink_nro ./scripts/build-switch.sh -DNSL_DIAGNOSTICS=ON -DNSL_DEFAULT_DUALSENSE=ON

# Builds, snapshots and sends the very same NRO; SWITCH_IP is always explicit.
switch-deploy:
	@test -n "$(SWITCH_IP)" || { echo 'Usage: make switch-deploy SWITCH_IP=<device address>'; exit 2; }
	./scripts/deploy-switch.sh --address "$(SWITCH_IP)"
