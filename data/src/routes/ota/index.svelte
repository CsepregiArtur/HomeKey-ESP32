<script lang="ts">
	import { onMount } from 'svelte';

	type Info = {
		version: string;
		project: string;
		target: string;
		chip_revision: number;
		partition: string;
		slot: string;
		slot_size: number;
		ota_available: boolean;
		update_size: number;
	};

	let info = $state<Info | null>(null);
	let error = $state('');
	let file = $state<File | null>(null);

	let uploading = $state(false);
	let progress = $state(0);
	let message = $state('');
	let failure = $state('');

	async function loadInfo() {
		try {
			const r = await fetch('/api/ota/info');
			if (!r.ok) throw new Error(`${r.status}`);
			info = await r.json();
		} catch (e) {
			error = e instanceof Error ? e.message : String(e);
		}
	}

	function pick(event: Event) {
		const input = event.target as HTMLInputElement;
		file = input.files?.[0] ?? null;
		// A previous attempt is no longer relevant once a different file is chosen.
		message = '';
		failure = '';
		progress = 0;
	}

	/**
	 * Upload the image in a single streaming request.
	 *
	 * The device needs Content-Length to size the write, so the whole body is sent as one
	 * request rather than chunked. XMLHttpRequest is used instead of fetch because it is
	 * the only browser API that reports upload progress, and an update that takes tens of
	 * seconds with no feedback looks like a hang.
	 */
	function upload() {
		if (!file || !info) return;

		if (file.size > info.slot_size) {
			failure = `The image is ${file.size} bytes but the application slot holds ${info.slot_size}. Build a smaller image or move to the single-slot layout.`;
			return;
		}

		uploading = true;
		progress = 0;
		message = '';
		failure = '';

		const request = new XMLHttpRequest();
		request.open('POST', '/api/ota/firmware');
		request.setRequestHeader('Content-Type', 'application/octet-stream');
		request.upload.onprogress = (event) => {
			if (event.lengthComputable) {
				progress = Math.round((event.loaded / event.total) * 100);
			}
		};
		request.onload = () => {
			// The device reboots as soon as it answers, so stop reporting progress: the
			// socket is about to drop and every later event is a false failure.
			uploading = false;
			if (request.status >= 200 && request.status < 300) {
				progress = 100;
				message =
					'Firmware installed. The device is rebooting into the new image — reload this page in about 15 seconds.';
			} else {
				try {
					failure = JSON.parse(request.responseText).error || `HTTP ${request.status}`;
				} catch {
					failure = `HTTP ${request.status}`;
				}
			}
		};
		request.onerror = () => {
			uploading = false;
			failure =
				'The connection dropped during the upload. An interrupted upload is not applied, so the device is still running the firmware it had.';
		};
		request.send(file);
	}

	onMount(loadInfo);
</script>

<div class="p-6 max-w-3xl">
	<h1 class="text-2xl font-bold mb-4">Firmware update</h1>

	{#if error}
		<div class="alert alert-error mb-4">{error}</div>
	{/if}

	{#if info}
		<div class="card bg-base-200 mb-4">
			<div class="card-body gap-1">
				<div class="flex justify-between py-1">
					<span class="font-semibold">Firmware version</span><code>{info.version}</code>
				</div>
				<div class="flex justify-between py-1">
					<span class="font-semibold">Chip</span>
					<span>
						<code>{info.target}</code>
						<span class="opacity-70"> (revision {info.chip_revision})</span>
					</span>
				</div>
				<div class="flex justify-between py-1">
					<span class="font-semibold">Running from</span>
					<span>
						<span class="badge badge-neutral mr-1">{info.partition}</span>
						<code>{info.slot}</code>
					</span>
				</div>
				<div class="flex justify-between py-1">
					<span class="font-semibold">Application slot</span>
					<span>{info.slot_size} B</span>
				</div>
			</div>
		</div>

		{#if !info.ota_available}
			<div class="alert alert-warning mb-4">
				This device has a single application slot, so there is nowhere to write an update.
				It must be flashed over a cable. See the single-slot layout documentation.
			</div>
		{/if}

		<div class="card bg-base-200">
			<div class="card-body">
				<h2 class="card-title text-lg">Install a firmware image</h2>
				<p class="text-sm opacity-70">
					Choose the <code>.bin</code> produced by the build (<code>build/HomeKey-ESP32.bin</code>).
					The update is written to the slot the device is <em>not</em> running from and only
					switched to after the image is validated, so an interrupted or corrupt upload leaves
					the current firmware untouched. If the new image fails to boot, the bootloader
					rolls back to the version you are running now.
				</p>
				<p class="text-sm opacity-70">
					Firmware only. This build has no separate filesystem image to install — the devices
					that need one are flashed over a cable.
				</p>

				<input
					type="file"
					class="file-input file-input-bordered w-full mt-2"
					accept=".bin,application/octet-stream"
					disabled={uploading || !info.ota_available}
					onchange={pick}
				/>

				{#if file}
					<div class="text-sm mt-2">
						<span class="font-semibold">{file.name}</span>
						<span class="opacity-70"> — {file.size} bytes</span>
						{#if info.update_size > 0 && file.size !== info.update_size}
							<span class="opacity-70"> (build produced {info.update_size})</span>
						{/if}
					</div>
				{/if}

				<div class="card-actions justify-end mt-2">
					<button class="btn btn-primary" disabled={uploading || !file || !info.ota_available} onclick={upload}>
						{#if uploading}
							<span class="loading loading-spinner loading-sm"></span>
							Uploading…
						{:else}
							Install firmware
						{/if}
					</button>
				</div>

				{#if uploading || progress > 0}
					<progress class="progress progress-primary w-full mt-2" value={progress} max="100"></progress>
				{/if}

				{#if failure}
					<div class="alert alert-error mt-2">{failure}</div>
				{/if}
				{#if message}
					<div class="alert alert-success mt-2">{message}</div>
				{/if}
			</div>
		</div>
	{/if}
</div>
