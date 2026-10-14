// Copyright © 2024. GothicKit Contributors
// SPDX-License-Identifier: MIT-Modern-Variant
#include "_Internal.h"

DmResult DmBand_create(DmBand** slf) {
	if (slf == NULL) {
		return DmResult_INVALID_ARGUMENT;
	}

	DmBand* new = *slf = Dm_alloc(sizeof *new);
	if (new == NULL) {
		return DmResult_MEMORY_EXHAUSTED;
	}

	new->reference_count = 1;
	return DmResult_SUCCESS;
}

DmBand* DmBand_retain(DmBand* slf) {
	if (slf == NULL) {
		return NULL;
	}

	(void) atomic_fetch_add(&slf->reference_count, 1);
	return slf;
}

void DmBand_release(DmBand* slf) {
	if (slf == NULL) {
		return;
	}

	size_t count = atomic_fetch_sub(&slf->reference_count, 1) - 1;
	if (count > 0) {
		return;
	}

	for (size_t i = 0; i < slf->instruments_len; ++i) {
		DmInstrument_free(&slf->instruments[i]);
	}

	Dm_free(slf->instruments);
	Dm_free(slf);
}

// A band instrument's patch is DirectMusic's MIDI patch (`dwPatch`): the program in bits 0-6, the
// bank select LSB in bits 8-14, the MSB in bits 16-22 and the drum kit flag in bit 31. A DLS
// instrument's bank (`ulBank`) is the MSB in bits 8-14 and the LSB in bits 0-6, with the drum kit
// flag in bit 31 as well, so `patch >> 8` lines the two banks up.
uint32_t DmInstrument_getBank(DmInstrument const* slf) {
	return (slf->patch >> 8) & 0x7F7FU;
}

uint32_t DmInstrument_getProgram(DmInstrument const* slf) {
	return slf->patch & 0x7FU;
}

bool DmInstrument_isDrumKit(DmInstrument const* slf) {
	return (slf->patch & 0x80000000U) != 0;
}

static DmDlsInstrument* DmInstrument_findDlsInstrument(DmInstrument* slf, uint32_t bank) {
	bool drum = DmInstrument_isDrumKit(slf);
	uint32_t patch = DmInstrument_getProgram(slf);

	// TODO(lmichaelis): Dirty fix for drum kit problems. Instead of choosing the first valid DLS instrument, we
	//                   chose the last valid one. This acts as if later instruments override previous ones and
	//                   thus prevents problems where the same channel is re-used multiple times, specifically in
	//                   Gothic 1, which assigns a drum kit and melodic instrument to the same channel. This works
	//                   in conjunction with the TSF creation in Dm_createHydra to prevent drum kit issues in G1.
	for (long long i = slf->dls->instrument_count - 1; i >= 0; --i) {
		DmDlsInstrument* ins = &slf->dls->instruments[i];
		bool ins_drum = (ins->bank & DmDls_DRUM_KIT) != 0;

		if (ins_drum == drum && (ins->bank & 0x7F7FU) == bank && ins->patch == patch) {
			return ins;
		}
	}

	return NULL;
}

DmDlsInstrument* DmInstrument_getDlsInstrument(DmInstrument* slf) {
	if (slf == NULL || slf->dls == NULL) {
		return NULL;
	}

	uint32_t bank = DmInstrument_getBank(slf);
	DmDlsInstrument* ins = DmInstrument_findDlsInstrument(slf, bank);

	// A GS synthesizer plays the capital tone (bank select MSB 0) of a variation it does not have.
	if (ins == NULL && (bank & 0x7F00U) != 0) {
		ins = DmInstrument_findDlsInstrument(slf, bank & 0x7FU);
	}

	if (ins != NULL) {
		return ins;
	}

	Dm_report(DmLogLevel_WARN,
	          "DmBand: Instrument patch %d:%d:%d%s not found in band '%s'",
	          bank >> 8,
	          bank & 0x7F,
	          DmInstrument_getProgram(slf),
	          DmInstrument_isDrumKit(slf) ? " (drums)" : "",
	          slf->reference.name);
	return NULL;
}

DmResult DmBand_download(DmBand* slf, DmLoader* loader) {
	if (slf == NULL || loader == NULL) {
		return DmResult_INVALID_ARGUMENT;
	}

	Dm_report(DmLogLevel_INFO, "DmBand: Downloading instruments for band '%s'", slf->info.unam);

	DmResult rv = DmResult_SUCCESS;
	for (size_t i = 0; i < slf->instruments_len; ++i) {
		DmInstrument* instrument = &slf->instruments[i];

		// The DLS has already been downloaded. We don't need to do it again.
		if (instrument->dls != NULL) {
			continue;
		}

		// If the patch is not valid, this instrument cannot be played since we don't know
		// where to find it in the DLS collection.
		if (!(instrument->options & (DmInstrument_VALID_PATCH | DmInstrument_VALID_BANKSELECT))) {
			Dm_report(DmLogLevel_DEBUG,
			          "DmBand: Not downloading instrument '%s' without valid patch",
			          instrument->reference.name);
			continue;
		}

		if (instrument->options & DmInstrument_GENERAL_MIDI) {
			instrument->reference.file = "gm.dls";
			Dm_report(DmLogLevel_INFO,
			          "DmBand: Trying to download instrument '%s' from the general MIDI collection (gm.dls)",
			          instrument->reference.name);
		} else if (instrument->options & DmInstrument_ROLAND_GS) {
			instrument->reference.file = "gs.dls";
			Dm_report(DmLogLevel_INFO,
			          "DmBand: Trying to download instrument '%s' from the Roland GS collection (gs.dls)",
			          instrument->reference.name);
		} else if (instrument->options & DmInstrument_YAMAHA_XG) {
			instrument->reference.file = "xg.dls";
			Dm_report(DmLogLevel_INFO,
			          "DmBand: Trying to download instrument '%s' from the Yamaha XG collection (xg.dls)",
			          instrument->reference.name);
		}

		rv = DmLoader_getDownloadableSound(loader, &instrument->reference, &instrument->dls);
		if (rv != DmResult_SUCCESS || instrument->dls == NULL) {
			continue;
		}

		DmDlsInstrument* dls_instrument = DmInstrument_getDlsInstrument(instrument);
		if (dls_instrument == NULL) {
			continue;
		}

		Dm_report(DmLogLevel_DEBUG,
		          "DmBand: DLS instrument '%s' assigned to channel %d for band '%s'",
		          dls_instrument->info.inam,
		          instrument->channel,
		          slf->info.unam);
	}

	return rv;
}

void DmInstrument_free(DmInstrument* slf) {
	if (slf == NULL || slf->dls == NULL) {
		return;
	}

	DmDls_release(slf->dls);
	slf->dls = NULL;
}
