<script lang="ts">
	/**
	 * First-run credential setup gate.
	 *
	 * A freshly flashed device keeps the shipped default credentials and comes up with
	 * `webAuthEnabled = false` and `setupCompleted = false`. Rather than ship known
	 * passwords or randomise secrets the user never gets to read, the firmware asks for
	 * every credential here, once, deliberately.
	 *
	 * Until this form is submitted there is no Web UI login, so the device must be kept
	 * on a trusted network.
	 */
	import { onMount } from 'svelte';
	import { route } from 'sv-router/generated';
	import { saveConfig } from '$lib/services/api';

	let { children } = $props();

	// The setup portal is reached *before* any credentials exist, so it must never be
	// gated - blocking it would make a factory-fresh device impossible to configure.
	const isCaptivePortal = $derived(route.pathname.startsWith('/captive-portal'));

	let loading = $state(true);
	let required = $state(false);
	let saving = $state(false);
	let error = $state<string | null>(null);

	let webUsername = $state('admin');
	let webPassword = $state('');
	let webPasswordConfirm = $state('');
	let setupCode = $state('');
	let apPassword = $state('HomeKey$123$');

	const weakCodes = new Set([
		'00000000', '11111111', '22222222', '33333333', '44444444', '55555555',
		'66666666', '77777777', '88888888', '99999999', '12345678', '87654321'
	]);

	function validate(): string | null {
		if (webUsername.trim().length === 0) return 'Web UI username is required.';
		if (webPassword.length < 8) return 'The Web UI password must be at least 8 characters.';
		if (webPassword !== webPasswordConfirm) return 'The two Web UI passwords do not match.';
		if (!/^\d{8}$/.test(setupCode)) return 'The HomeKit setup code must be exactly 8 digits.';
		if (weakCodes.has(setupCode)) return 'That HomeKit setup code is too easy to guess.';

		if (apPassword.length < 8) return 'The setup AP password must be at least 8 characters.';
		return null;
	}

	async function submit(e: SubmitEvent) {
		e.preventDefault();
		error = null;

		const problem = validate();
		if (problem) {
			error = problem;
			return;
		}

		saving = true;
		try {
			const result = await saveConfig('misc', {
				webAuthEnabled: true,
				webUsername: webUsername.trim(),
				webPassword,
				setupCode,
				accessPointPassword: apPassword,
				setupCompleted: true
			} as any);

			if (result.success) {
				// Web UI authentication is on from this point, so the reload will ask
				// for the credentials that were just chosen.
				window.location.reload();
			} else {
				error = (result as any).error ?? 'Could not save the credentials.';
			}
		} finally {
			saving = false;
		}
	}

	onMount(async () => {
		if (isCaptivePortal) return;
		try {
			const res = await fetch('/config?type=misc').then((r) => r.json());
			const misc = res?.data ?? {};
			required = misc.setupCompleted !== true;
			if (misc.webUsername) webUsername = misc.webUsername;
		} catch (err) {
			// Fail open. The rest of the UI reports its own errors, and blocking here
			// would leave the device unmanageable if the API hiccups during startup.
			console.warn('First-run setup check failed:', err);
			required = false;
		} finally {
			loading = false;
		}
	});
</script>

{#if !isCaptivePortal && loading}
	<div class="min-h-screen flex items-center justify-center bg-base-200">
		<span class="loading loading-spinner loading-lg"></span>
	</div>
{:else if !isCaptivePortal && required}
	<div class="min-h-screen flex items-center justify-center bg-base-200 p-4">
		<div class="card w-full max-w-2xl bg-base-100 shadow-xl">
			<div class="card-body gap-4">
				<div>
					<h2 class="card-title text-lg">Finish setting up this device</h2>
					<p class="text-sm text-base-content/70 mt-1">
						This device is still using the default credentials that are published with the
						firmware. Choose your own below. The Web UI has <strong>no password</strong> until
						you finish, so keep the device on a trusted network.
					</p>
				</div>

				{#if error}
					<div class="alert alert-error py-2 px-3">
						<span class="text-xs">{error}</span>
					</div>
				{/if}

				<form onsubmit={submit} class="space-y-5">
					<!-- Web UI -->
					<div class="space-y-3">
						<h3 class="text-sm font-semibold">Web UI login</h3>
						<div class="grid grid-cols-1 sm:grid-cols-3 gap-3">
							<div class="form-control">
								<label class="label" for="setup-username">
									<span class="label-text text-xs">Username</span>
								</label>
								<input
									id="setup-username"
									type="text"
									bind:value={webUsername}
									class="input input-sm input-bordered w-full"
								/>
							</div>
							<div class="form-control">
								<label class="label" for="setup-password">
									<span class="label-text text-xs">Password</span>
								</label>
								<input
									id="setup-password"
									type="password"
									bind:value={webPassword}
									placeholder="min 8 characters"
									autocomplete="new-password"
									class="input input-sm input-bordered w-full"
								/>
							</div>
							<div class="form-control">
								<label class="label" for="setup-password2">
									<span class="label-text text-xs">Confirm password</span>
								</label>
								<input
									id="setup-password2"
									type="password"
									bind:value={webPasswordConfirm}
									autocomplete="new-password"
									class="input input-sm input-bordered w-full"
								/>
							</div>
						</div>
					</div>

					<!-- HomeKit -->
					<div class="space-y-3">
						<h3 class="text-sm font-semibold">HomeKit setup code</h3>
						<div class="form-control max-w-xs">
							<label class="label" for="setup-code">
								<span class="label-text text-xs">8 digits, no leading zero</span>
							</label>
							<input
								id="setup-code"
								type="text"
								inputmode="numeric"
								maxlength="8"
								bind:value={setupCode}
								placeholder="12345678"
								class="input input-sm input-bordered w-full font-mono tracking-widest"
							/>
						</div>
						<p class="text-xs text-base-content/60">
							This is the code you type into Apple Home. It is public until you change it.
						</p>
					</div>

					<!-- Setup AP -->
					<div class="space-y-3">
						<h3 class="text-sm font-semibold">Setup access point password</h3>
						<div class="form-control max-w-xs">
							<label class="label" for="setup-ap">
								<span class="label-text text-xs">min 8 characters</span>
							</label>
							<input
								id="setup-ap"
								type="password"
								bind:value={apPassword}
								autocomplete="new-password"
								class="input input-sm input-bordered w-full"
							/>
						</div>
						<p class="text-xs text-base-content/60">
							Used by the <span class="font-mono">HK_*</span> setup network. Takes effect the
							next time that network starts.
						</p>
					</div>

					<div class="card-actions justify-end pt-2">
						<button type="submit" class="btn btn-primary btn-sm" disabled={saving}>
							{#if saving}
								<span class="loading loading-spinner loading-xs"></span>
							{/if}
							Save and continue
						</button>
					</div>
				</form>
			</div>
		</div>
	</div>
{:else}
	{@render children()}
{/if}
