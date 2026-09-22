<script lang="ts">
	import { onMount } from 'svelte';

	let data = $state<any>(null);
	let error = $state('');
	let loading = $state(true);

	onMount(async () => {
		try {
			const r = await fetch('/health');
			if (!r.ok) throw new Error((await r.json()).error || `${r.status}`);
			data = await r.json();
		} catch (e: any) {
			error = e.message;
		} finally {
			loading = false;
		}
	});
</script>

<div class="p-6 max-w-3xl">
	<h1 class="text-2xl font-bold mb-4">Health</h1>

	{#if loading}
		<div class="skeleton h-40 w-full"></div>
	{:else if error}
		<div class="alert alert-error">{error}</div>
	{:else}
		<div class="card bg-base-200">
			<div class="card-body gap-1">
				<div class="flex justify-between py-1"><span class="font-semibold">Network</span><span>{data.network}</span></div>
				<div class="flex justify-between py-1"><span class="font-semibold">MQTT</span><span>{data.mqtt}</span></div>
				<div class="flex justify-between py-1"><span class="font-semibold">NFC</span><span>{data.nfc}</span></div>
				<div class="flex justify-between py-1"><span class="font-semibold">Backup</span><span>{data.backup}</span></div>
				<div class="flex justify-between py-1"><span class="font-semibold">Certificate</span><span>{data.certificate}</span></div>
			</div>
		</div>

		<div class="card bg-base-200 mt-4">
			<div class="card-body gap-1">
				<div class="flex justify-between py-1"><span class="font-semibold">Firmware</span><span>{data.firmware_version}</span></div>
				<div class="flex justify-between py-1"><span class="font-semibold">Uptime (s)</span><span>{data.uptime}</span></div>
				<div class="flex justify-between py-1"><span class="font-semibold">Free heap</span><span>{data.free_heap}</span></div>
				<div class="flex justify-between py-1"><span class="font-semibold">Reset reason</span><span>{data.reset_reason}</span></div>
				<div class="flex justify-between py-1"><span class="font-semibold">Lock</span><span>{data.lock_current} → {data.lock_target}</span></div>
			</div>
		</div>

		<div class="card bg-base-200 mt-4">
			<div class="card-body gap-1">
				<h2 class="font-bold">Security</h2>
				<div class="py-1">all_ok: {data.security?.all_ok ? 'yes' : 'no'}</div>
				<pre class="text-xs whitespace-pre-wrap">{data.security?.warnings || '—'}</pre>
			</div>
		</div>
	{/if}
</div>
