<script lang="ts">
	import { onMount } from 'svelte';

	let status = $state<any>(null);
	let error = $state('');
	let exported = $state('');
	let exporting = $state(false);
	let backupHex = $state('');
	let secretHex = $state('');
	let restoring = $state(false);
	let restored = $state('');

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

	async function loadFile(event: Event) {
		const input = event.target as HTMLInputElement;
		const file = input.files?.[0];
		if (file) backupHex = (await file.text()).trim();
	}

	async function restore() {
		restoring = true;
		restored = '';
		error = '';
		try {
			const r = await fetch('/backup/restore', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ secret: secretHex.trim(), backup: backupHex.trim() })
			});
			const res = await r.json();
			if (!r.ok) throw new Error(res.error || `${r.status}`);
			restored = res.message || 'Restore completed';
		} catch (e: any) {
			error = e.message;
		} finally {
			restoring = false;
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

	<div class="card bg-base-200 mt-6">
		<div class="card-body gap-3">
			<h2 class="card-title text-lg">Restore from a backup</h2>
			<p class="text-sm opacity-80">
				Restores household membership and configuration from an encrypted backup. The
				replacement node gets a new identity of its own; it is never a clone of the node
				that made the backup.
			</p>
			<p class="text-sm opacity-80">
				It needs the <strong>recovery secret</strong> you stored when you exported it — the
				backup cannot be decrypted without it. It also refuses to run on a device that
				already has its own identity, so it belongs on the replacement unit.
			</p>

			<div class="form-control">
				<label class="label" for="restore-secret"><span class="label-text">Recovery secret (hex)</span></label>
				<input
					id="restore-secret"
					class="input input-bordered font-mono text-xs"
					bind:value={secretHex}
					placeholder="the secret shown when you exported it"
				/>
			</div>

			<div class="form-control">
				<label class="label" for="restore-file"><span class="label-text">Backup — choose the .hex file, or paste it</span></label>
				<input
					id="restore-file"
					type="file"
					class="file-input file-input-bordered file-input-sm w-full"
					accept=".hex,text/plain"
					onchange={loadFile}
				/>
				<textarea
					id="restore-backup"
					rows={4}
					class="textarea textarea-bordered font-mono text-xs mt-2"
					bind:value={backupHex}
					placeholder="paste the backup here"
				></textarea>
			</div>

			<div>
				<button
					class="btn btn-warning"
					disabled={restoring || !backupHex || !secretHex}
					onclick={restore}
				>
					{restoring ? 'Restoring…' : 'Restore this node'}
				</button>
			</div>

			{#if restored}
				<div class="alert alert-success">
					<span>{restored}. Reboot the device to apply the new identity cleanly.</span>
				</div>
			{/if}
		</div>
	</div>
</div>
