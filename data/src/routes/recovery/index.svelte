<script lang="ts">
	import { onMount } from 'svelte';

	let status = $state<any>(null);
	let error = $state('');
	let exported = $state('');
	let exporting = $state(false);

	async function load() {
		try {
			const r = await fetch('/household');
			if (!r.ok) throw new Error((await r.json()).error || `${r.status}`);
			status = await r.json();
		} catch (e: any) {
			error = e.message;
		}
	}

	async function exportOnce() {
		exporting = true;
		exported = '';
		try {
			const r = await fetch('/recovery/export', { method: 'POST' });
			const res = await r.json();
			if (!r.ok) throw new Error(res.error || `${r.status}`);
			exported = res.recovery_secret;
		} catch (e: any) {
			error = e.message;
		} finally {
			exporting = false;
			await load();
		}
	}

	onMount(load);
</script>

<div class="p-6 max-w-3xl">
	<h1 class="text-2xl font-bold mb-4">Recovery</h1>

	<div class="alert alert-warning mb-4">
		<svg xmlns="http://www.w3.org/2000/svg" class="h-6 w-6 shrink-0 stroke-current" fill="none" viewBox="0 0 24 24">
			<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" />
		</svg>
		<span>Household membership and device configuration are restored from the encrypted
			backup. The replacement NFC/HomeKey reader receives a <strong>new reader identity</strong> —
			existing Apple Home HomeKey credentials are <strong>not</strong> restored and must be
			re-provisioned in the Apple Home app.</span>
	</div>

	{#if error}
		<div class="alert alert-error">{error}</div>
	{/if}

	<div class="card bg-base-200">
		<div class="card-body gap-1">
			<div class="flex justify-between py-1">
				<span class="font-semibold">Recovery secret</span>
				<span>{status?.has_recovery_secret ? 'created' : 'none'}</span>
			</div>
			<div class="flex justify-between py-1">
				<span class="font-semibold">Exported</span>
				<span>{status?.recovery_exported ? 'yes' : 'no'}</span>
			</div>
		</div>
	</div>

	<div class="mt-4">
		<button class="btn btn-primary" disabled={exporting || status?.recovery_exported} onclick={exportOnce}>
			{exporting ? 'Exporting…' : 'Export recovery secret (one time)'}
		</button>
		<p class="text-sm opacity-70 mt-2">
			The recovery secret is shown once. Store it offline — it decrypts household backups and
			enables restoring a replacement node.
		</p>
	</div>

	{#if exported}
		<div class="card bg-base-200 mt-4">
			<div class="card-body">
				<p class="text-sm opacity-70 mb-1">Recovery secret (copy and store securely):</p>
				<code class="text-xs break-all">{exported}</code>
			</div>
		</div>
	{/if}
</div>
