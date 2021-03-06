
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "../../file_io.h"
#include "../../user_io.h"

#include "pcecd.h"

#define PCECD_DATA_IO_INDEX 2

pcecdd_t pcecdd;

pcecdd_t::pcecdd_t() {
	latency = 10;
	loaded = 0;
	index = 0;
	lba = 0;
	scanOffset = 0;
	isData = 1;
	state = PCECD_STATE_NODISC;
	audioLength = 0;
	audioOffset = 0;
	SendData = NULL;
	has_status = 0;
	data_req = false;
	can_read_next = false;
	CDDAStart = 0;
	CDDAEnd = 0;
	CDDAMode = PCECD_CDDAMODE_SILENT;

	stat[0] = 0x0;
	stat[1] = 0x0;
}

static int sgets(char *out, int sz, char **in)
{
	*out = 0;
	do
	{
		char *instr = *in;
		int cnt = 0;

		while (*instr && *instr != 10)
		{
			if (*instr == 13)
			{
				instr++;
				continue;
			}

			if (cnt < sz - 1)
			{
				out[cnt++] = *instr;
				out[cnt] = 0;
			}

			instr++;
		}

		if(*instr == 10) instr++;
		*in = instr;
	}
	while (!*out && **in);

	return *out;
}

int pcecdd_t::LoadCUE(const char* filename) {
	static char fname[1024 + 10];
	static char line[128];
	char *ptr, *lptr;
	static char toc[100 * 1024];
	int hdr = 0;

	strcpy(fname, filename);

	memset(toc, 0, sizeof(toc));
	if (!FileLoad(fname, toc, sizeof(toc) - 1)) return 1;

	printf("\x1b[32mPCECD: Open CUE: %s\n\x1b[0m", fname);

	int mm, ss, bb, pregap = 0;

	char *buf = toc;
	while (sgets(line, sizeof(line), &buf))
	{
		lptr = line;
		while (*lptr == 0x20) lptr++;

		/* decode FILE commands */
		if (!(memcmp(lptr, "FILE", 4)))
		{
			ptr = fname + strlen(fname) - 1;
			while ((ptr - fname) && (*ptr != '/') && (*ptr != '\\')) ptr--;
			if (ptr - fname) ptr++;

			lptr += 4;
			while (*lptr == 0x20) lptr++;

			if (*lptr == '\"')
			{
				lptr++;
				while ((*lptr != '\"') && (lptr <= (line + 128)) && (ptr < (fname + 1023)))
					*ptr++ = *lptr++;
			}
			else
			{
				while ((*lptr != 0x20) && (lptr <= (line + 128)) && (ptr < (fname + 1023)))
					*ptr++ = *lptr++;
			}
			*ptr = 0;

			if(!FileOpen(&this->toc.tracks[this->toc.last].f, fname)) return -1;

			printf("\x1b[32mPCECD: Open track file: %s\n\x1b[0m", fname);

			int len = strlen(fname);
			hdr = (len > 4 && !strcasecmp(fname + len - 4, ".wav")) ? 44 : 0;

			pregap = 0;

			this->toc.tracks[this->toc.last].offset = 0;

			if (!strstr(lptr, "BINARY") && !strstr(lptr, "MOTOROLA") && !strstr(lptr, "WAVE"))
			{
				FileClose(&this->toc.tracks[this->toc.last].f);
				printf("\x1b[32mPCECD: unsupported file: %s\n\x1b[0m", fname);

				return -1;
			}
		}

		/* decode TRACK commands */
		else if ((sscanf(lptr, "TRACK %02d %*s", &bb)) || (sscanf(lptr, "TRACK %d %*s", &bb)))
		{
			if (bb != (this->toc.last + 1))
			{
				FileClose(&this->toc.tracks[this->toc.last].f);
				printf("\x1b[32mPCECD: missing tracks: %s\n\x1b[0m", fname);
				break;
			}

			//if (!this->toc.last)
			{
				if (strstr(lptr, "MODE1/2048"))
				{
					this->toc.tracks[this->toc.last].sector_size = 2048;
					this->toc.tracks[this->toc.last].type = 1;
				}
				else if (strstr(lptr, "MODE1/2352"))
				{
					this->toc.tracks[this->toc.last].sector_size = 2352;
					this->toc.tracks[this->toc.last].type = 1;

					FileSeek(&this->toc.tracks[this->toc.last].f, 0x10, SEEK_SET);
				}
				else if (strstr(lptr, "AUDIO"))
				{
					this->toc.tracks[this->toc.last].sector_size = 2352;
					this->toc.tracks[this->toc.last].type = 0;

					FileSeek(&this->toc.tracks[this->toc.last].f, 0, SEEK_SET);
				}

				/*if (this->sectorSize)
				{
					this->toc.tracks[0].type = 1;

					FileReadAdv(&this->toc.tracks[0].f, header, 0x210);
					FileSeek(&this->toc.tracks[0].f, 0, SEEK_SET);
				}*/
			}

			if (this->toc.last)
			{
				if (!this->toc.tracks[this->toc.last].f.opened())
				{
					this->toc.tracks[this->toc.last - 1].end = 0;
				}
			}
		}

		/* decode PREGAP commands */
		else if (sscanf(lptr, "PREGAP %02d:%02d:%02d", &mm, &ss, &bb) == 3)
		{
			pregap += bb + ss * 75 + mm * 60 * 75;
		}

		/* decode INDEX commands */
		else if ((sscanf(lptr, "INDEX 00 %02d:%02d:%02d", &mm, &ss, &bb) == 3) ||
			(sscanf(lptr, "INDEX 0 %02d:%02d:%02d", &mm, &ss, &bb) == 3))
		{
			if (this->toc.last && !this->toc.tracks[this->toc.last - 1].end)
			{
				this->toc.tracks[this->toc.last - 1].end = bb + ss * 75 + mm * 60 * 75 + pregap;
			}
		}
		else if ((sscanf(lptr, "INDEX 01 %02d:%02d:%02d", &mm, &ss, &bb) == 3) ||
			(sscanf(lptr, "INDEX 1 %02d:%02d:%02d", &mm, &ss, &bb) == 3))
		{
			if (!this->toc.tracks[this->toc.last].f.opened())
			{
				FileOpen(&this->toc.tracks[this->toc.last].f, fname);
				this->toc.tracks[this->toc.last].start = bb + ss * 75 + mm * 60 * 75 + pregap;
				this->toc.tracks[this->toc.last].offset = (pregap * this->toc.tracks[this->toc.last].sector_size) - hdr;
				if (this->toc.last && !this->toc.tracks[this->toc.last - 1].end)
				{
					this->toc.tracks[this->toc.last - 1].end = this->toc.tracks[this->toc.last].start;
				}
			}
			else
			{
				FileSeek(&this->toc.tracks[this->toc.last].f, 0, SEEK_SET);

				this->toc.tracks[this->toc.last].start = this->toc.end + pregap;
				this->toc.tracks[this->toc.last].offset = (this->toc.tracks[this->toc.last].start * this->toc.tracks[this->toc.last].sector_size) - hdr;
				this->toc.tracks[this->toc.last].end = this->toc.tracks[this->toc.last].start + ((this->toc.tracks[this->toc.last].f.size - hdr + this->toc.tracks[this->toc.last].sector_size - 1) / this->toc.tracks[this->toc.last].sector_size);

				this->toc.tracks[this->toc.last].start += (bb + ss * 75 + mm * 60 * 75);
				this->toc.end = this->toc.tracks[this->toc.last].end;
			}

			this->toc.last++;
			if (this->toc.last == 99) break;
		}
	}

	if (this->toc.last && !this->toc.tracks[this->toc.last - 1].end)
	{
		this->toc.end += pregap;
		this->toc.tracks[this->toc.last - 1].end = this->toc.end;
	}

	for (int i = 0; i < this->toc.last; i++)
	{
		printf("\x1b[32mPCECD: Track = %u, start = %u, end = %u, offset = %d, sector_size=%d, type = %u\n\x1b[0m", i, this->toc.tracks[i].start, this->toc.tracks[i].end, this->toc.tracks[i].offset, this->toc.tracks[i].sector_size, this->toc.tracks[i].type);
	}

	FileClose(&this->toc.tracks[this->toc.last].f);
	return 0;
}

int pcecdd_t::Load(const char *filename)
{
	Unload();

	if (LoadCUE(filename)) return -1;

	if (this->toc.last)
	{
		this->toc.tracks[this->toc.last].start = this->toc.end;
		this->loaded = 1;

		//memcpy(&fname[strlen(fname) - 4], ".sub", 4);
		//this->toc.sub = fopen(getFullPath(fname), "r");

		printf("\x1b[32mPCECD: CD mounted , last track = %u\n\x1b[0m", this->toc.last);
		return 1;
	}

	return 0;
}

void pcecdd_t::Unload()
{
	if (this->loaded)
	{
		for (int i = 0; i < this->toc.last; i++)
		{
			FileClose(&this->toc.tracks[i].f);
		}

		//if (this->toc.sub) fclose(this->toc.sub);

		this->loaded = 0;
	}

	memset(&this->toc, 0x00, sizeof(this->toc));
}

void pcecdd_t::Reset() {
	latency = 10;
	index = 0;
	lba = 0;
	scanOffset = 0;
	isData = 1;
	state = loaded ? PCECD_STATE_IDLE : PCECD_STATE_NODISC;
	audioLength = 0;
	audioOffset = 0;
	has_status = 0;
	data_req = false;
	can_read_next = false;
	CDDAStart = 0;
	CDDAEnd = 0;
	CDDAMode = PCECD_CDDAMODE_SILENT;

	stat[0] = 0x0;
	stat[1] = 0x0;
}

void pcecdd_t::Update() {
	if (this->state == PCECD_STATE_READ)
	{
		DISKLED_ON;
		if (this->latency > 0)
		{
			this->latency--;
			return;
		}

		if (this->index >= this->toc.last)
		{
			this->state = PCECD_STATE_IDLE;
			return;
		}

		if (!this->can_read_next)
			return;

		this->can_read_next = false;

		//if (this->toc.sub) mcd_sub_send();

		if (this->toc.tracks[this->index].type)
		{
			// CD-ROM (Mode 1)
			sec_buf[0] = 0x00;
			sec_buf[1] = 0x08 | 0x80;
			ReadData(sec_buf + 2);

			if (SendData)
				SendData(sec_buf, 2048 + 2, PCECD_DATA_IO_INDEX);

			printf("\x1b[32mPCECD: Data sector send = %i\n\x1b[0m", this->lba);
		}
		else
		{
			if (this->lba >= this->toc.tracks[this->index].start)
			{
				this->isData = 0x00;
			}

			//SectorSend(0);
		}

		this->cnt--;

		if (!this->cnt) {
			stat[0] = 0;
			stat[1] = 0;
			has_status = 1;

			this->state = PCECD_STATE_IDLE;
		}
		else {

		}

		this->lba++;
		if (this->lba >= this->toc.tracks[this->index].end)
		{
			this->index++;

			this->isData = 0x01;

			if (this->toc.tracks[this->index].f.opened())
			{
				FileSeek(&this->toc.tracks[this->index].f, (this->toc.tracks[this->index].start * 2352) - this->toc.tracks[this->index].offset, SEEK_SET);
			}
		}
	}
	else if (this->state == PCECD_STATE_PLAY)
	{
		DISKLED_ON;
		if (this->latency > 0)
		{
			this->latency--;
			return;
		}

		if (this->index >= this->toc.last)
		{
			this->state = PCECD_STATE_IDLE;
			return;
		}

		if (this->toc.tracks[this->index].type)
			return;

		FileSeek(&this->toc.tracks[index].f, (this->lba * 2352) - this->toc.tracks[index].offset, SEEK_SET);

		sec_buf[0] = 0x30;
		sec_buf[1] = 0x09;
		ReadCDDA(sec_buf + 2);

		if (SendData)
			SendData(sec_buf, 2352 + 2, PCECD_DATA_IO_INDEX);

		//printf("\x1b[32mPCECD: Audio sector send = %i, track = %i, offset = %i\n\x1b[0m", this->lba, this->index, (this->lba * 2352) - this->toc.tracks[index].offset);

		this->lba++;
		if (this->lba > this->CDDAEnd)
		{
			if (this->CDDAMode == PCECD_CDDAMODE_LOOP) {
				this->lba = this->CDDAStart;
			}
			else {
				this->state = PCECD_STATE_IDLE;
			}
		}

	}
}

void pcecdd_t::CommandExec() {
	msf_t msf;
	int new_lba = 0;
	static uint8_t buf[32];

	memset(buf, 0, 32);

	switch (comm[0]) {
	case PCECD_COMM_TESTUNIT:
		if (state == PCECD_STATE_NODISC) {
			CommandError(SENSEKEY_NOT_READY, NSE_NO_DISC, 0, 0);
			PendStatus(PCECD_STATUS_CHECK_COND, 0);
		}
		else {
			PendStatus(PCECD_STATUS_GOOD, 0);
		}

		printf("\x1b[32mPCECD: Command TESTUNIT, state = %u\n\x1b[0m", state);
		break;

	case PCECD_COMM_REQUESTSENSE:
		buf[0] = 18;
		buf[1] = 0 | 0x80;

		buf[2] = 0x70;
		buf[4] = sense.key;
		buf[9] = 0x0A;
		buf[14] = sense.asc;
		buf[15] = sense.ascq;
		buf[16] = sense.fru;

		sense.key = sense.asc = sense.ascq = sense.fru = 0;

		PendStatus(PCECD_STATUS_GOOD, 0);

		printf("\x1b[32mPCECD: Command REQUESTSENSE, key = %02X, asc = %02X, ascq = %02X, fru = %02X\n\x1b[0m", sense.key, sense.asc, sense.ascq, sense.fru);

		if (SendData)
			SendData(buf, 18+2, PCECD_DATA_IO_INDEX);

		break;

	case PCECD_COMM_GETDIRINFO: {
		int len = 0;
		switch (comm[1]) {
		case 0:
		default:
			buf[0] = 2;
			buf[1] = 0 | 0x80;
			buf[2] = 1;
			buf[3] = BCD(this->toc.last);
			len = 2 + 2;
			break;

		case 1:
			new_lba = this->toc.end + 150;
			LBAToMSF(new_lba, &msf);

			buf[0] = 4;
			buf[1] = 0 | 0x80;
			buf[2] = BCD(msf.m);
			buf[3] = BCD(msf.s);
			buf[4] = BCD(msf.f);
			buf[5] = 0;
			len = 4 + 2;
			break;

		case 2:
			int track = U8(comm[2]);
			new_lba = this->toc.tracks[track - 1].start + 150;
			LBAToMSF(new_lba, &msf);

			buf[0] = 4;
			buf[1] = 0 | 0x80;
			buf[2] = BCD(msf.m);
			buf[3] = BCD(msf.s);
			buf[4] = BCD(msf.f);
			buf[5] = this->toc.tracks[track - 1].type << 2;
			len = 4 + 2;
			break;
		}

		PendStatus(PCECD_STATUS_GOOD, 0);

		printf("\x1b[32mPCECD: Command GETDIRINFO, [1] = %02X, [2] = %02X(%d)\n\x1b[0m", comm[1], comm[2], comm[2]);

		if (SendData && len)
			SendData(buf, len, PCECD_DATA_IO_INDEX);

		printf("\x1b[32mPCECD: Send data, len = %u, [2] = %02X, [3] = %02X, [4] = %02X, [5] = %02X\n\x1b[0m", len, buf[2], buf[3], buf[4], buf[5]);
	}
		break;

	case PCECD_COMM_READ6: {
		new_lba = ((comm[1] << 16) | (comm[2] << 8) | comm[3]) & 0x1FFFFF;
		int cnt_ = comm[4];

		this->lba = new_lba;
		this->cnt = cnt_;

		int index = GetTrackByLBA(new_lba, &this->toc);

		this->index = index;
		if (new_lba < this->toc.tracks[index].start)
		{
			new_lba = this->toc.tracks[index].start;
		}

		if (this->toc.tracks[index].f.opened())
		{
			int offset = (new_lba * this->toc.tracks[index].sector_size) - this->toc.tracks[index].offset;
			FileSeek(&this->toc.tracks[index].f, offset, SEEK_SET);
		}

		this->audioOffset = 0;

		//if (this->toc.sub) fseek(this->toc.sub, lba_ * 96, SEEK_SET);

		this->can_read_next = true;
		this->state = PCECD_STATE_READ;

		printf("\x1b[32mPCECD: Command READ6, lba = %u, cnt = %u\n\x1b[0m", this->lba, this->cnt);
	}
		break;

	case PCECD_COMM_MODESELECT6:
		if (comm[4]) {
			data_req = true;
		}
		else {
			PendStatus(PCECD_STATUS_GOOD, 0);
		}

		printf("\x1b[32mPCECD: Command MODESELECT6, cnt = %u\n\x1b[0m", comm[4]);

		break;

	case PCECD_COMM_SAPSP: {
		switch (comm[9] & 0xc0)
		{
		default:
		case 0x00:
			new_lba = (comm[3] << 16) | (comm[4] << 8) | comm[5];
			break;

		case 0x40:
			MSFToLBA(&new_lba, U8(comm[2]), U8(comm[3]), U8(comm[4]));
			break;

		case 0x80:
		{
			int track = U8(comm[2]);

			if (!track)
				track = 1;
			else if (track > toc.last)
				track = toc.last;
			new_lba = this->toc.tracks[track - 1].start;
		}
		break;
		}


		this->lba = new_lba;
		int index = GetTrackByLBA(new_lba, &this->toc);

		this->index = index;
		/*if (lba_ < this->toc.tracks[index].start)
		{
			lba_ = this->toc.tracks[index].start;
		}*/

		this->CDDAStart = new_lba;
		this->CDDAEnd = this->toc.tracks[index].end;
		this->CDDAMode = comm[1];

		printf("PCECD_COMM_SAPSP: CDDAEnd=%d\n", this->CDDAEnd);

		if (this->CDDAMode != PCECD_CDDAMODE_SILENT) {
			this->state = PCECD_STATE_PLAY;
		}

		FileSeek(&this->toc.tracks[index].f, (this->lba * 2352) - this->toc.tracks[index].offset, SEEK_SET);

		sec_buf[0] = 0x30;
		sec_buf[1] = 0x09;
		ReadCDDA(sec_buf + 2);

		if (SendData)
			SendData(sec_buf, 2352 + 2, PCECD_DATA_IO_INDEX);

		this->lba++;

		PendStatus(PCECD_STATUS_GOOD, 0);
	}
		printf("\x1b[32mPCECD: Command SAPSP, start = %i, [1] = %02X, [2] = %02X, [9] = %02X\n\x1b[0m", this->CDDAStart, comm[1], comm[2], comm[9]);
		break;

	case PCECD_COMM_SAPEP: {
		switch (comm[9] & 0xc0)
		{
		default:
		case 0x00:
			new_lba = (comm[3] << 16) | (comm[4] << 8) | comm[5];
			break;

		case 0x40:
			MSFToLBA(&new_lba, U8(comm[2]), U8(comm[3]), U8(comm[4]));
			break;

		case 0x80:
		{
			int track = U8(comm[2]);

			if (!track)	track = 1;
			new_lba = (track >= toc.last) ? this->toc.end : (this->toc.tracks[track - 1].start + 150);
		}
		break;
		}

		this->CDDAMode = comm[1];
		this->CDDAEnd = new_lba;

		printf("PCECD_COMM_SAPEP: CDDAEnd=%d\n", this->CDDAEnd);

		if (this->CDDAMode != PCECD_CDDAMODE_SILENT) {
			this->state = PCECD_STATE_PLAY;
		}

		PendStatus(PCECD_STATUS_GOOD, 0);
	}
		printf("\x1b[32mPCECD: Command SAPEP, end = %i, [1] = %02X, [2] = %02X, [9] = %02X\n\x1b[0m", this->CDDAEnd, comm[1], comm[2], comm[9]);
		break;

	case PCECD_COMM_PAUSE: {
		this->state = PCECD_STATE_PAUSE;

		PendStatus(PCECD_STATUS_GOOD, 0);
	}
		printf("\x1b[32mPCECD: Command PAUSE, current lba = %i\n\x1b[0m", this->lba);
		break;

	case PCECD_COMM_READSUBQ: {
		int lba_rel = this->toc.tracks[this->index].start - this->toc.tracks[this->index].offset + 150;
		new_lba = this->toc.tracks[this->index].start + 150;

		buf[0] = 0x0A;
		buf[1] = 0 | 0x80;
		buf[2] = this->state == PCECD_STATE_PLAY ? 0 : 3;
		buf[3] = 0;
		buf[4] = this->index + 1;
		buf[5] = this->index;

		LBAToMSF(lba_rel, &msf);
		buf[6] = BCD(msf.m);
		buf[7] = BCD(msf.s);
		buf[8] = BCD(msf.f);

		LBAToMSF(new_lba, &msf);
		buf[9] = BCD(msf.m);
		buf[10] = BCD(msf.s);
		buf[11] = BCD(msf.f);

		PendStatus(PCECD_STATUS_GOOD, 0);

		printf("\x1b[32mPCECD: Command READSUBQ, [1] = %02X, track = %i, index = %i, lba_rel = %i, lba_abs = %i\n\x1b[0m", comm[1], this->index + 1, this->index, lba_rel, new_lba);

		if (SendData)
			SendData(buf, 10 + 2, PCECD_DATA_IO_INDEX);
	}
		break;

	default:
		//stat[0] = this->status;

		printf("\x1b[32mPCECD: Command undefined, [0] = %02X, [1] = %02X, [2] = %02X, [3] = %02X, [4] = %02X, [5] = %02X\n\x1b[0m", comm[0], comm[1], comm[2], comm[3], comm[4], comm[5]);
		break;
	}
}

int pcecdd_t::GetStatus(uint8_t* buf) {
	memcpy(buf, stat, 2);
	return 0;
}

int pcecdd_t::SetCommand(uint8_t* buf) {
	memcpy(comm, buf, 12);
	return 0;
}

void pcecdd_t::PendStatus(uint8_t status, uint8_t message) {
	stat[0] = status;
	stat[1] = message;
	has_status = 1;
}

void pcecdd_t::LBAToMSF(int lba, msf_t* msf) {
	msf->m = (lba / 75) / 60;
	msf->s = (lba / 75) % 60;
	msf->f = (lba % 75);
}

void pcecdd_t::MSFToLBA(int* lba, uint8_t m, uint8_t s, uint8_t f) {
	*lba = f + s * 75 + m * 60 * 75;
}

void pcecdd_t::MSFToLBA(int* lba, msf_t* msf) {
	*lba = msf->f + msf->s * 75 + msf->m * 60 * 75;
}

int pcecdd_t::GetTrackByLBA(int lba, toc_t* toc) {
	int index = 0;
	while ((toc->tracks[index].end <= lba) && (index < toc->last)) index++;
	return index;
}

void pcecdd_t::ReadData(uint8_t *buf)
{
	if (this->toc.tracks[this->index].type && (this->lba >= 0))
	{
		if (this->toc.tracks[this->index].sector_size == 2048)
		{
			FileSeek(&this->toc.tracks[this->index].f, this->lba * 2048 - this->toc.tracks[this->index].offset, SEEK_SET);
		}
		else
		{
			FileSeek(&this->toc.tracks[this->index].f, this->lba * 2352 + 16 - this->toc.tracks[this->index].offset, SEEK_SET);
		}

		FileReadAdv(&this->toc.tracks[this->index].f, buf, 2048);
	}
}

int pcecdd_t::ReadCDDA(uint8_t *buf)
{
	this->audioLength = 2352;// 2352 + 2352 - this->audioOffset;
	this->audioOffset = 0;// 2352;

	if (this->toc.tracks[this->index].f.opened())
	{
		FileReadAdv(&this->toc.tracks[this->index].f, buf, this->audioLength);
	}

	return this->audioLength;
}

void pcecdd_t::ReadSubcode(uint16_t* buf)
{
	(void)buf;
	/*
	uint8_t subc[96];
	int i, j, n;

	fread(subc, 96, 1, this->toc.sub);

	for (i = 0, n = 0; i < 96; i += 2, n++)
	{
		int code = 0;
		for (j = 0; j < 8; j++)
		{
			int bits = (subc[(j * 12) + (i / 8)] >> (6 - (i & 6))) & 3;
			code |= ((bits & 1) << (7 - j));
			code |= ((bits >> 1) << (15 - j));
		}

		buf[n] = code;
	}
	*/
}

int pcecdd_t::SectorSend(uint8_t* header)
{
	uint8_t buf[2352 + 2352];
	int len = 2352;

	if (header) {
		memcpy(buf + 12, header, 4);
		ReadData(buf + 16);
	}
	else {
		len = ReadCDDA(buf);
	}

	if (SendData)
		return SendData(buf, len, PCECD_DATA_IO_INDEX);

	return 0;
}

void pcecdd_t::CommandError(uint8_t key, uint8_t asc, uint8_t ascq, uint8_t fru) {
	sense.key = key;
	sense.asc = asc;
	sense.ascq = ascq;
	sense.fru = fru;
}




