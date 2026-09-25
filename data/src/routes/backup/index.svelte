<script lang="ts">
	import { onMount } from 'svelte';

	let status = $state<any>(null);
	let error = $state('');
	let creating = $state(false);
	let backupHex = $state('');
	let downloaded = $state(false);

	async function loadStatus() {
		try {
			const r = await fetch('/backup');
			if (!r.ok) throw new Error((await r.json()).error || `${r.status}`);
			status = await r.json();
		} catch (e: any) {
			error = e.message;
		}
	}

	async function createBackup() {
		creating = true;
		backupHex = '';
		try {
			const r = await fetch('/backup/create', { method: 'POST' });
			if (!r.ok) throw new Error((await r.json()).error || `${r.status}`);
			const res = await r.json();
			backupHex = res.backup;
		} catch (e: any) {
			error = e.message;
		} finally {
			creating = false;
			await loadStatus();
		}
	}

	function download() {
		if (!backupHex) return;
		const blob = new Blob([backupHex], { type: 'text/plain' });
		const a = document.createElement('a');
		a.href = URL.createObjectURL(blob);
		a.download = `homekey-backup-${Date.now()}.hex`;
		a.click();
		URL.revokeObjectURL(a.href);
		downloaded = true;
	}

	// The device keeps only the time and hash of the last backup, so an unsaved one is lost
	// the moment this page goes away. Asking before that happens is the last chance to say so.
	$effect(() => {
		const guard = (event: BeforeUnloadEvent) => {
			if (backupHex && !downloaded) event.preventDefault();
		};
		window.addEventListener('beforeunload', guard);
		return () => window.removeEventListener('beforeunload', guard);
	});

	onMount(loadStatus);
</script>

<div class="p-6 max-w-3xl">
	<h1 class="text-2xl font-bold mb-4">Backup</h1>

	{#if error}
		<div class="alert alert-error">{error}</div>
	{/if}

	<div class="card bg-base-200">
		<div class="card-body gap-1">
			<div class="flex justify-between py-1">
				<span class="font-semibold">Last backup</span>
				<span>{status?.last_backup_time ? new Date(status.last_backup_time * 1000).toLocaleString() : 'never'}</span>
			</div>
			<div class="py-1">
				<span class="font-semibold block mb-1">Last backup hash</span>
				<code class="text-xs break-all">{status?.last_backup_hash || '—'}</code>
			</div>
		</div>
	</div>

	<div class="mt-4 flex gap-2">
		<button class="btn btn-primary" disabled={creating} onclick={createBackup}>
			{creating ? 'Creating…' : 'Create encrypted backup'}
		</button>
		{#if backupHex}
			<button class="btn btn-primary" onclick={download}>Download it now</button>
		{/if}
	</div>

	<!-- "Completed" on the node and in Home Assistant means a backup was produced, not that one
	     is stored anywhere: nothing keeps a copy, and the status topic stays "completed"
	     afterwards whether or not anyone saved the file. -->
	<div class="alert alert-warning mt-4">
		<span>The device keeps <strong>no copy</strong>. It records only the time and hash of the
			last backup, so this is the one opportunity to save it — leave this page without
			downloading and the backup is gone, and a new one has to be made.</span>
	</div>

	{#if backupHex}
		<div class="card bg-base-200 mt-4">
			<div class="card-body">
				<p class="text-sm opacity-70 mb-1">Encrypted backup (hex) — this is the only copy:</p>
				<textarea readonly rows={6} class="textarea textarea-bordered text-xs font-mono">{backupHex}</textarea>
			</div>
		</div>
	{/if}
</div>
