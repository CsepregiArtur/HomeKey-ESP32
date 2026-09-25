<script lang="ts">
	import { onMount } from 'svelte';

	let data = $state<any>(null);
	let error = $state('');
	let loading = $state(true);

	onMount(async () => {
		try {
			const r = await fetch('/household');
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
	<h1 class="text-2xl font-bold mb-4">Household</h1>

	{#if loading}
		<div class="skeleton h-40 w-full"></div>
	{:else if error}
		<div class="alert alert-error">{error}</div>
	{:else}
		<div class="card bg-base-200">
			<div class="card-body gap-1">
				<div class="flex justify-between py-1">
					<span class="font-semibold">Name</span><span>{data.household_name || '—'}</span>
				</div>
				<div class="flex justify-between py-1">
					<span class="font-semibold">Household ID</span><code>{data.household_id || '—'}</code>
				</div>
				<div class="flex justify-between py-1">
					<span class="font-semibold">Status</span>
					<span class:badge-success={data.state === 'ACTIVE'} class:badge-warning={data.state === 'PROVISIONING'} class="badge">{data.state}</span>
				</div>
				<div class="flex justify-between py-1">
					<span class="font-semibold">Config version</span><span>{data.config_version}</span>
				</div>
				<div class="flex justify-between py-1">
					<span class="font-semibold">Recovery secret</span>
					<span>{data.has_recovery_secret ? (data.recovery_exported ? 'stored (exported)' : 'stored (not exported yet)') : 'none'}</span>
				</div>
				<!-- The salt is public (it is a KDF input, not a key) and is shown because the
				     household control key is derived from the secret *and* the salt together: a
				     client given only the secret derives a key that can never match. -->
				<div class="flex justify-between py-1">
					<span class="font-semibold">Recovery salt</span>
					<code class="text-xs">{data.recovery_salt || '—'}</code>
				</div>
				<div class="flex justify-between py-1">
					<span class="font-semibold">Trust key</span>
					<code class="text-xs">{data.trust_key || '—'}</code>
				</div>
			</div>
		</div>

		<div class="mt-4 flex gap-2">
			<a href="/node" class="btn btn-outline btn-sm">Node</a>
			<a href="/backup" class="btn btn-outline btn-sm">Backup</a>
			<a href="/recovery" class="btn btn-outline btn-sm">Recovery</a>
			<a href="/provision" class="btn btn-outline btn-sm">Provision</a>
		</div>
	{/if}
</div>
