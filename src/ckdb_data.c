/*
 * Copyright 1995-2014 Andrew Smith
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "ckdb.h"

// Clear text printable version of txt up to first '\0'
char *safe_text(char *txt)
{
	unsigned char *ptr = (unsigned char *)txt;
	size_t len;
	char *ret, *buf;

	if (!txt) {
		buf = strdup("(Null)");
		if (!buf)
			quithere(1, "malloc OOM");
		return buf;
	}

	// Allocate the maximum needed
	len = (strlen(txt)+1)*4+1;
	ret = buf = malloc(len);
	if (!buf)
		quithere(1, "malloc (%d) OOM", (int)len);
	while (*ptr) {
		if (*ptr >= ' ' && *ptr <= '~')
			*(buf++) = *(ptr++);
		else {
			snprintf(buf, 5, "0x%02x", *(ptr++));
			buf += 4;
		}
	}
	strcpy(buf, "0x00");
	return ret;
}

void _txt_to_data(enum data_type typ, char *nam, char *fld, void *data, size_t siz, WHERE_FFL_ARGS)
{
	char *tmp;

	switch (typ) {
		case TYPE_STR:
			// A database field being bigger than local storage is a fatal error
			if (siz < (strlen(fld)+1)) {
				quithere(1, "Field %s structure size %d is smaller than db %d" WHERE_FFL,
						nam, (int)siz, (int)strlen(fld)+1, WHERE_FFL_PASS);
			}
			strcpy((char *)data, fld);
			break;
		case TYPE_BIGINT:
			if (siz != sizeof(int64_t)) {
				quithere(1, "Field %s bigint incorrect structure size %d - should be %d"
						WHERE_FFL,
						nam, (int)siz, (int)sizeof(int64_t), WHERE_FFL_PASS);
			}
			*((long long *)data) = atoll(fld);
			break;
		case TYPE_INT:
			if (siz != sizeof(int32_t)) {
				quithere(1, "Field %s int incorrect structure size %d - should be %d"
						WHERE_FFL,
						nam, (int)siz, (int)sizeof(int32_t), WHERE_FFL_PASS);
			}
			*((int32_t *)data) = atoi(fld);
			break;
		case TYPE_TV:
			if (siz != sizeof(tv_t)) {
				quithere(1, "Field %s tv_t incorrect structure size %d - should be %d"
						WHERE_FFL,
						nam, (int)siz, (int)sizeof(tv_t), WHERE_FFL_PASS);
			}
			unsigned int yyyy, mm, dd, HH, MM, SS, uS = 0, tz, tzm = 0;
			char pm[2];
			struct tm tm;
			time_t tim;
			int n;
			// A timezone looks like: +10 or +09:30 or -05 etc
			n = sscanf(fld, "%u-%u-%u %u:%u:%u%1[+-]%u:%u",
					&yyyy, &mm, &dd, &HH, &MM, &SS, pm, &tz, &tzm);
			if (n < 8) {
				// allow uS
				n = sscanf(fld, "%u-%u-%u %u:%u:%u.%u%1[+-]%u:%u",
						&yyyy, &mm, &dd, &HH, &MM, &SS, &uS, pm, &tz, &tzm);
				if (n < 9) {
					quithere(1, "Field %s tv_t unhandled date '%s' (%d)" WHERE_FFL,
						 nam, fld, n, WHERE_FFL_PASS);
				}

				if (n < 10)
					tzm = 0;
			} else if (n < 9)
				tzm = 0;
			tm.tm_sec = (int)SS;
			tm.tm_min = (int)MM;
			tm.tm_hour = (int)HH;
			tm.tm_mday = (int)dd;
			tm.tm_mon = (int)mm - 1;
			tm.tm_year = (int)yyyy - 1900;
			tm.tm_isdst = -1;
			tim = timegm(&tm);
			if (tim > COMPARE_EXPIRY) {
				((tv_t *)data)->tv_sec = default_expiry.tv_sec;
				((tv_t *)data)->tv_usec = default_expiry.tv_usec;
			} else {
				tz = tz * 60 + tzm;
				// time was converted ignoring tz - so correct it
				if (pm[0] == '-')
					tim += 60 * tz;
				else
					tim -= 60 * tz;
				((tv_t *)data)->tv_sec = tim;
				((tv_t *)data)->tv_usec = uS;
			}
			break;
		case TYPE_CTV:
			if (siz != sizeof(tv_t)) {
				quithere(1, "Field %s tv_t incorrect structure size %d - should be %d"
						WHERE_FFL,
						nam, (int)siz, (int)sizeof(tv_t), WHERE_FFL_PASS);
			}
			long sec, nsec;
			int c;
			// Caller test for tv_sec=0 for failure
			((tv_t *)data)->tv_sec = 0L;
			((tv_t *)data)->tv_usec = 0L;
			c = sscanf(fld, "%ld,%ld", &sec, &nsec);
			if (c > 0) {
				((tv_t *)data)->tv_sec = (time_t)sec;
				if (c > 1)
					((tv_t *)data)->tv_usec = (nsec + 500) / 1000;
				if (((tv_t *)data)->tv_sec >= COMPARE_EXPIRY) {
					((tv_t *)data)->tv_sec = default_expiry.tv_sec;
					((tv_t *)data)->tv_usec = default_expiry.tv_usec;
				}
			}
			break;
		case TYPE_BLOB:
			tmp = strdup(fld);
			if (!tmp) {
				quithere(1, "Field %s (%d) OOM" WHERE_FFL,
						nam, (int)strlen(fld), WHERE_FFL_PASS);
			}
			*((char **)data) = tmp;
			break;
		case TYPE_DOUBLE:
			if (siz != sizeof(double)) {
				quithere(1, "Field %s int incorrect structure size %d - should be %d"
						WHERE_FFL,
						nam, (int)siz, (int)sizeof(double), WHERE_FFL_PASS);
			}
			*((double *)data) = atof(fld);
			break;
		default:
			quithere(1, "Unknown field %s (%d) to convert" WHERE_FFL,
					nam, (int)typ, WHERE_FFL_PASS);
			break;
	}
}

// N.B. STRNCPY* macros truncate, whereas this aborts ckdb if src > trg
void _txt_to_str(char *nam, char *fld, char data[], size_t siz, WHERE_FFL_ARGS)
{
	_txt_to_data(TYPE_STR, nam, fld, (void *)data, siz, WHERE_FFL_PASS);
}

void _txt_to_bigint(char *nam, char *fld, int64_t *data, size_t siz, WHERE_FFL_ARGS)
{
	_txt_to_data(TYPE_BIGINT, nam, fld, (void *)data, siz, WHERE_FFL_PASS);
}

void _txt_to_int(char *nam, char *fld, int32_t *data, size_t siz, WHERE_FFL_ARGS)
{
	_txt_to_data(TYPE_INT, nam, fld, (void *)data, siz, WHERE_FFL_PASS);
}

void _txt_to_tv(char *nam, char *fld, tv_t *data, size_t siz, WHERE_FFL_ARGS)
{
	_txt_to_data(TYPE_TV, nam, fld, (void *)data, siz, WHERE_FFL_PASS);
}

// Convert msg S,nS to tv_t
void _txt_to_ctv(char *nam, char *fld, tv_t *data, size_t siz, WHERE_FFL_ARGS)
{
	_txt_to_data(TYPE_CTV, nam, fld, (void *)data, siz, WHERE_FFL_PASS);
}

void _txt_to_blob(char *nam, char *fld, char **data, WHERE_FFL_ARGS)
{
	_txt_to_data(TYPE_BLOB, nam, fld, (void *)data, 0, WHERE_FFL_PASS);
}

void _txt_to_double(char *nam, char *fld, double *data, size_t siz, WHERE_FFL_ARGS)
{
	_txt_to_data(TYPE_DOUBLE, nam, fld, (void *)data, siz, WHERE_FFL_PASS);
}

char *_data_to_buf(enum data_type typ, void *data, char *buf, size_t siz, WHERE_FFL_ARGS)
{
	struct tm tm;

	if (!buf) {
		switch (typ) {
			case TYPE_STR:
			case TYPE_BLOB:
				siz = strlen((char *)data) + 1;
				break;
			case TYPE_BIGINT:
				siz = BIGINT_BUFSIZ;
				break;
			case TYPE_INT:
				siz = INT_BUFSIZ;
				break;
			case TYPE_TV:
			case TYPE_TVS:
				siz = DATE_BUFSIZ;
				break;
			case TYPE_CTV:
				siz = CDATE_BUFSIZ;
				break;
			case TYPE_DOUBLE:
				siz = DOUBLE_BUFSIZ;
				break;
			default:
				quithere(1, "Unknown field (%d) to convert" WHERE_FFL,
						(int)typ, WHERE_FFL_PASS);
				break;
		}

		buf = malloc(siz);
		if (!buf)
			quithere(1, "(%d) OOM" WHERE_FFL, (int)siz, WHERE_FFL_PASS);
	}

	switch (typ) {
		case TYPE_STR:
		case TYPE_BLOB:
			snprintf(buf, siz, "%s", (char *)data);
			break;
		case TYPE_BIGINT:
			snprintf(buf, siz, "%"PRId64, *((uint64_t *)data));
			break;
		case TYPE_INT:
			snprintf(buf, siz, "%"PRId32, *((uint32_t *)data));
			break;
		case TYPE_TV:
			gmtime_r(&(((tv_t *)data)->tv_sec), &tm);
			snprintf(buf, siz, "%d-%02d-%02d %02d:%02d:%02d.%06ld+00",
					   tm.tm_year + 1900,
					   tm.tm_mon + 1,
					   tm.tm_mday,
					   tm.tm_hour,
					   tm.tm_min,
					   tm.tm_sec,
					   (((tv_t *)data)->tv_usec));
			break;
		case TYPE_CTV:
			snprintf(buf, siz, "%ld,%ld",
					   (((tv_t *)data)->tv_sec),
					   (((tv_t *)data)->tv_usec));
			break;
		case TYPE_TVS:
			snprintf(buf, siz, "%ld", (((tv_t *)data)->tv_sec));
			break;
		case TYPE_DOUBLE:
			snprintf(buf, siz, "%f", *((double *)data));
			break;
	}

	return buf;
}

char *_str_to_buf(char data[], char *buf, size_t siz, WHERE_FFL_ARGS)
{
	return _data_to_buf(TYPE_STR, (void *)data, buf, siz, WHERE_FFL_PASS);
}

char *_bigint_to_buf(int64_t data, char *buf, size_t siz, WHERE_FFL_ARGS)
{
	return _data_to_buf(TYPE_BIGINT, (void *)(&data), buf, siz, WHERE_FFL_PASS);
}

char *_int_to_buf(int32_t data, char *buf, size_t siz, WHERE_FFL_ARGS)
{
	return _data_to_buf(TYPE_INT, (void *)(&data), buf, siz, WHERE_FFL_PASS);
}

char *_tv_to_buf(tv_t *data, char *buf, size_t siz, WHERE_FFL_ARGS)
{
	return _data_to_buf(TYPE_TV, (void *)data, buf, siz, WHERE_FFL_PASS);
}

// Convert tv to S,uS
char *_ctv_to_buf(tv_t *data, char *buf, size_t siz, WHERE_FFL_ARGS)
{
	return _data_to_buf(TYPE_CTV, (void *)data, buf, siz, WHERE_FFL_PASS);
}

// Convert tv to seconds (ignore uS)
char *_tvs_to_buf(tv_t *data, char *buf, size_t siz, WHERE_FFL_ARGS)
{
	return _data_to_buf(TYPE_TVS, (void *)data, buf, siz, WHERE_FFL_PASS);
}

/* unused yet
char *_blob_to_buf(char *data, char *buf, size_t siz, WHERE_FFL_ARGS)
{
	return _data_to_buf(TYPE_BLOB, (void *)data, buf, siz, WHERE_FFL_PASS);
}
*/

char *_double_to_buf(double data, char *buf, size_t siz, WHERE_FFL_ARGS)
{
	return _data_to_buf(TYPE_DOUBLE, (void *)(&data), buf, siz, WHERE_FFL_PASS);
}

// TODO: do this better ... :)
void dsp_hash(char *hash, char *buf, size_t siz)
{
	char *ptr;

	ptr = hash + strlen(hash) - (siz - 1) - 8;
	if (ptr < hash)
		ptr = hash;
	STRNCPYSIZ(buf, ptr, siz);
}

// For mutiple variable function calls that need the data
char *_transfer_data(K_ITEM *item, WHERE_FFL_ARGS)
{
	TRANSFER *transfer;
	char *mvalue;

	if (!item) {
		quitfrom(1, file, func, line,
			 "Attempt to use NULL transfer item");
	}
	if (item->name != transfer_free->name) {
		quitfrom(1, file, func, line,
			 "Attempt to cast item '%s' data as '%s'",
			 item->name,
			 transfer_free->name);
	}
	transfer = (TRANSFER *)(item->data);
	if (!transfer) {
		quitfrom(1, file, func, line,
			 "Transfer item has NULL data");
	}
	mvalue = transfer->mvalue;
	if (!mvalue) {
		/* N.B. name and svalue strings will have \0 termination
		 * even if they are both corrupt, since mvalue is NULL */
		quitfrom(1, file, func, line,
			 "Transfer '%s' '%s' has NULL mvalue",
			 transfer->name, transfer->svalue);
	}
	return mvalue;
}

void dsp_transfer(K_ITEM *item, FILE *stream)
{
	TRANSFER *t;

	if (!item)
		fprintf(stream, "%s() called with (null) item\n", __func__);
	else {
		DATA_TRANSFER(t, item);
		fprintf(stream, " name='%s' mvalue='%s' malloc=%c\n",
				t->name, t->mvalue,
				(t->svalue == t->mvalue) ? 'N' : 'Y');
	}
}

// order by name asc
cmp_t cmp_transfer(K_ITEM *a, K_ITEM *b)
{
	TRANSFER *ta, *tb;
	DATA_TRANSFER(ta, a);
	DATA_TRANSFER(tb, b);
	return CMP_STR(ta->name, tb->name);
}

K_ITEM *find_transfer(K_TREE *trf_root, char *name)
{
	TRANSFER transfer;
	K_TREE_CTX ctx[1];
	K_ITEM look;

	STRNCPY(transfer.name, name);
	INIT_TRANSFER(&look);
	look.data = (void *)(&transfer);
	return find_in_ktree(trf_root, &look, cmp_transfer, ctx);
}

K_ITEM *_optional_name(K_TREE *trf_root, char *name, int len, char *patt,
			char *reply, size_t siz, WHERE_FFL_ARGS)
{
	TRANSFER *trf;
	K_ITEM *item;
	char *mvalue;
	regex_t re;
	size_t dlen;
	int ret;

	reply[0] = '\0';

	item = find_transfer(trf_root, name);
	if (!item)
		return NULL;

	DATA_TRANSFER(trf, item);
	mvalue = trf->mvalue;
	if (mvalue)
		dlen = strlen(mvalue);
	else
		dlen = 0;
	if (!mvalue || (int)dlen < len) {
		if (!mvalue) {
			LOGERR("%s(): field '%s' NULL (%d) from %s():%d",
				__func__, name, (int)dlen, len, func, line);
		} else
			snprintf(reply, siz, "failed.short %s", name);
		return NULL;
	}

	if (patt) {
		if (regcomp(&re, patt, REG_NOSUB) != 0) {
			snprintf(reply, siz, "failed.REG %s", name);
			return NULL;
		}

		ret = regexec(&re, mvalue, (size_t)0, NULL, 0);
		regfree(&re);

		if (ret != 0) {
			snprintf(reply, siz, "failed.invalid %s", name);
			return NULL;
		}
	}

	return item;
}

K_ITEM *_require_name(K_TREE *trf_root, char *name, int len, char *patt,
			char *reply, size_t siz, WHERE_FFL_ARGS)
{
	TRANSFER *trf;
	K_ITEM *item;
	char *mvalue;
	regex_t re;
	size_t dlen;
	int ret;

	reply[0] = '\0';

	item = find_transfer(trf_root, name);
	if (!item) {
		LOGERR("%s(): failed, field '%s' missing from %s():%d",
			__func__, name, func, line);
		snprintf(reply, siz, "failed.missing %s", name);
		return NULL;
	}

	DATA_TRANSFER(trf, item);
	mvalue = trf->mvalue;
	if (mvalue)
		dlen = strlen(mvalue);
	else
		dlen = 0;
	if (!mvalue || (int)dlen < len) {
		LOGERR("%s(): failed, field '%s' short (%s%d<%d) from %s():%d",
			__func__, name, mvalue ? EMPTY : "null",
			(int)dlen, len, func, line);
		snprintf(reply, siz, "failed.short %s", name);
		return NULL;
	}

	if (patt) {
		if (regcomp(&re, patt, REG_NOSUB) != 0) {
			LOGERR("%s(): failed, field '%s' failed to"
				" compile patt from %s():%d",
				__func__, name, func, line);
			snprintf(reply, siz, "failed.REG %s", name);
			return NULL;
		}

		ret = regexec(&re, mvalue, (size_t)0, NULL, 0);
		regfree(&re);

		if (ret != 0) {
			LOGERR("%s(): failed, field '%s' invalid from %s():%d",
				__func__, name, func, line);
			snprintf(reply, siz, "failed.invalid %s", name);
			return NULL;
		}
	}

	return item;
}

// order by userid asc,workername asc
cmp_t cmp_workerstatus(K_ITEM *a, K_ITEM *b)
{
	WORKERSTATUS *wa, *wb;
	DATA_WORKERSTATUS(wa, a);
	DATA_WORKERSTATUS(wb, b);
	cmp_t c = CMP_BIGINT(wa->userid, wb->userid);
	if (c == 0)
		c = CMP_STR(wa->workername, wb->workername);
	return c;
}

/* TODO: replace a lot of the code for all data types that codes finds,
 *  each with specific functions for finding, to centralise the finds,
 *  with passed ctx's */
K_ITEM *get_workerstatus(int64_t userid, char *workername)
{
	WORKERSTATUS workerstatus;
	K_TREE_CTX ctx[1];
	K_ITEM look, *find;

	workerstatus.userid = userid;
	STRNCPY(workerstatus.workername, workername);

	INIT_WORKERSTATUS(&look);
	look.data = (void *)(&workerstatus);
	K_RLOCK(workerstatus_free);
	find = find_in_ktree(workerstatus_root, &look, cmp_workerstatus, ctx);
	K_RUNLOCK(workerstatus_free);
	return find;
}

/* Worker loading/creation calls this with create = true
 * All others with create = false since the workerstatus should exist
 * Failure is a code bug and a reported error, but handled anyway
 * This has 2 sets of file/func/line to allow 2 levels of traceback
 */
K_ITEM *_find_create_workerstatus(int64_t userid, char *workername,
				  bool create, const char *file2,
				  const char *func2, const int line2,
				  WHERE_FFL_ARGS)
{
	WORKERSTATUS *row;
	K_ITEM *item;

	item = get_workerstatus(userid, workername);
	if (!item) {
		if (!create) {
			LOGEMERG("%s(): Missing workerstatus %"PRId64"/%s"
				 WHERE_FFL WHERE_FFL,
				 __func__, userid, workername,
				 file2, func2, line2, WHERE_FFL_PASS);
			return NULL;
		}

		K_WLOCK(workerstatus_free);
		item = k_unlink_head(workerstatus_free);

		DATA_WORKERSTATUS(row, item);

		bzero(row, sizeof(*row));
		row->userid = userid;
		STRNCPY(row->workername, workername);

		workerstatus_root = add_to_ktree(workerstatus_root, item, cmp_workerstatus);
		k_add_head(workerstatus_store, item);
		K_WUNLOCK(workerstatus_free);
	}
	return item;
}

// default tree order by username asc,expirydate desc
cmp_t cmp_users(K_ITEM *a, K_ITEM *b)
{
	USERS *ua, *ub;
	DATA_USERS(ua, a);
	DATA_USERS(ub, b);
	cmp_t c = CMP_STR(ua->username, ub->username);
	if (c == 0)
		c = CMP_TV(ub->expirydate, ua->expirydate);
	return c;
}

// order by userid asc,expirydate desc
cmp_t cmp_userid(K_ITEM *a, K_ITEM *b)
{
	USERS *ua, *ub;
	DATA_USERS(ua, a);
	DATA_USERS(ub, b);
	cmp_t c = CMP_BIGINT(ua->userid, ub->userid);
	if (c == 0)
		c = CMP_TV(ub->expirydate, ua->expirydate);
	return c;
}

// Must be R or W locked before call
K_ITEM *find_users(char *username)
{
	USERS users;
	K_TREE_CTX ctx[1];
	K_ITEM look;

	STRNCPY(users.username, username);
	users.expirydate.tv_sec = default_expiry.tv_sec;
	users.expirydate.tv_usec = default_expiry.tv_usec;

	INIT_USERS(&look);
	look.data = (void *)(&users);
	return find_in_ktree(users_root, &look, cmp_users, ctx);
}

// Must be R or W locked before call
K_ITEM *find_userid(int64_t userid)
{
	USERS users;
	K_TREE_CTX ctx[1];
	K_ITEM look;

	users.userid = userid;
	users.expirydate.tv_sec = default_expiry.tv_sec;
	users.expirydate.tv_usec = default_expiry.tv_usec;

	INIT_USERS(&look);
	look.data = (void *)(&users);
	return find_in_ktree(userid_root, &look, cmp_userid, ctx);
}

// TODO: endian? (to avoid being all zeros?)
void make_salt(USERS *users)
{
	long int r1, r2, r3, r4;

	r1 = random();
	r2 = random();
	r3 = random();
	r4 = random();

	__bin2hex(users->salt, (void *)(&r1), 4);
	__bin2hex(users->salt+8, (void *)(&r2), 4);
	__bin2hex(users->salt+16, (void *)(&r3), 4);
	__bin2hex(users->salt+24, (void *)(&r4), 4);
}

void password_hash(char *username, char *passwordhash, char *salt, char *result, size_t siz)
{
	char tohash[TXT_BIG+1];
	char buf[TXT_BIG+1];
	size_t len, tot;
	char why[1024];

	if (siz < SHA256SIZHEX+1) {
		snprintf(why, sizeof(why),
			 "target result too small (%d/%d)",
			 (int)siz, SHA256SIZHEX+1);
		goto hashfail;
	}

	if (sizeof(buf) < SHA256SIZBIN) {
		snprintf(why, sizeof(why),
			 "temporary target buf too small (%d/%d)",
			 (int)sizeof(buf), SHA256SIZBIN);
		goto hashfail;
	}

	tot = len = strlen(passwordhash) / 2;
	if (len != SHA256SIZBIN) {
		snprintf(why, sizeof(why),
			 "passwordhash wrong size (%d/%d)",
			 (int)len, SHA256SIZBIN);
		goto hashfail;
	}
	if (len > sizeof(tohash)) {
		snprintf(why, sizeof(why),
			 "temporary tohash too small (%d/%d)",
			 (int)sizeof(tohash), (int)len);
		goto hashfail;
	}

	hex2bin(tohash, passwordhash, len);

	len = strlen(salt) / 2;
	if (len != SALTSIZBIN) {
		snprintf(why, sizeof(why),
			 "salt wrong size (%d/%d)",
			 (int)len, SALTSIZBIN);
		goto hashfail;
	}
	if ((tot + len) > sizeof(tohash)) {
		snprintf(why, sizeof(why),
			 "passwordhash+salt too big (%d/%d)",
			 (int)(tot + len), (int)sizeof(tohash));
		goto hashfail;
	}

	hex2bin(tohash+tot, salt, len);
	tot += len;

	sha256((const unsigned char *)tohash, (unsigned int)tot, (unsigned char *)buf);

	__bin2hex(result, (void *)buf, SHA256SIZBIN);

	return;
hashfail:
	LOGERR("%s() Failed to hash '%s' password: %s",
		__func__, username, why);
	result[0] = '\0';
}

bool check_hash(USERS *users, char *passwordhash)
{
	char hex[SHA256SIZHEX+1];

	if (*(users->salt)) {
		password_hash(users->username, passwordhash, users->salt, hex, sizeof(hex));
		return (strcasecmp(hex, users->passwordhash) == 0);
	} else
		return (strcasecmp(passwordhash, users->passwordhash) == 0);
}

// default tree order by userid asc,attname asc,expirydate desc
cmp_t cmp_useratts(K_ITEM *a, K_ITEM *b)
{
	USERATTS *ua, *ub;
	DATA_USERATTS(ua, a);
	DATA_USERATTS(ub, b);
	cmp_t c = CMP_BIGINT(ua->userid, ub->userid);
	if (c == 0) {
		c = CMP_STR(ua->attname, ub->attname);
		if (c == 0)
			c = CMP_TV(ub->expirydate, ua->expirydate);
	}
	return c;
}

// Must be R or W locked before call
K_ITEM *find_useratts(int64_t userid, char *attname)
{
	USERATTS useratts;
	K_TREE_CTX ctx[1];
	K_ITEM look;

	useratts.userid = userid;
	STRNCPY(useratts.attname, attname);
	useratts.expirydate.tv_sec = default_expiry.tv_sec;
	useratts.expirydate.tv_usec = default_expiry.tv_usec;

	INIT_USERATTS(&look);
	look.data = (void *)(&useratts);
	return find_in_ktree(useratts_root, &look, cmp_useratts, ctx);
}

// order by userid asc,workername asc,expirydate desc
cmp_t cmp_workers(K_ITEM *a, K_ITEM *b)
{
	WORKERS *wa, *wb;
	DATA_WORKERS(wa, a);
	DATA_WORKERS(wb, b);
	cmp_t c = CMP_BIGINT(wa->userid, wb->userid);
	if (c == 0) {
		c = CMP_STR(wa->workername, wb->workername);
		if (c == 0)
			c = CMP_TV(wb->expirydate, wa->expirydate);
	}
	return c;
}

K_ITEM *find_workers(int64_t userid, char *workername)
{
	WORKERS workers;
	K_TREE_CTX ctx[1];
	K_ITEM look;

	workers.userid = userid;
	STRNCPY(workers.workername, workername);
	workers.expirydate.tv_sec = default_expiry.tv_sec;
	workers.expirydate.tv_usec = default_expiry.tv_usec;

	INIT_WORKERS(&look);
	look.data = (void *)(&workers);
	return find_in_ktree(workers_root, &look, cmp_workers, ctx);
}

K_ITEM *new_worker(PGconn *conn, bool update, int64_t userid, char *workername,
		   char *diffdef, char *idlenotificationenabled,
		   char *idlenotificationtime, char *by,
		   char *code, char *inet, tv_t *cd, K_TREE *trf_root)
{
	K_ITEM *item;

	item = find_workers(userid, workername);
	if (item) {
		if (!confirm_sharesummary && update) {
			workers_update(conn, item, diffdef, idlenotificationenabled,
				       idlenotificationtime, by, code, inet, cd,
				       trf_root, true);
		}
	} else {
		if (confirm_sharesummary) {
			// Shouldn't be possible since the sharesummary is already aged
			LOGERR("%s() %"PRId64"/%s workername not found during confirm",
				__func__, userid, workername);
			return NULL;
		}

		// TODO: limit how many?
		item = workers_add(conn, userid, workername, diffdef,
				   idlenotificationenabled, idlenotificationtime,
				   by, code, inet, cd, trf_root);
	}
	return item;
}

K_ITEM *new_default_worker(PGconn *conn, bool update, int64_t userid, char *workername,
			  char *by, char *code, char *inet, tv_t *cd, K_TREE *trf_root)
{
	return new_worker(conn, update, userid, workername, DIFFICULTYDEFAULT_DEF_STR,
			  IDLENOTIFICATIONENABLED_DEF, IDLENOTIFICATIONTIME_DEF_STR,
			  by, code, inet, cd, trf_root);
}

/* unused
static K_ITEM *new_worker_find_user(PGconn *conn, bool update, char *username,
				    char *workername, char *diffdef,
				    char *idlenotificationenabled,
				    char *idlenotificationtime,
				    char *by, char *code, char *inet,
				    tv_t *cd, K_TREE *trf_root)
{
	K_ITEM *item;
	USERS *users;

	K_RLOCK(users_free);
	item = find_users(username);
	K_RUNLOCK(users_free);
	if (!item)
		return NULL;

	DATA_USERS(users, item);
	return new_worker(conn, update, users->userid, workername, diffdef,
			  idlenotificationenabled, idlenotificationtime,
			  by, code, inet, cd, trf_root);
}
*/

// order by userid asc,expirydate desc,payaddress asc
cmp_t cmp_paymentaddresses(K_ITEM *a, K_ITEM *b)
{
	PAYMENTADDRESSES *pa, *pb;
	DATA_PAYMENTADDRESSES(pa, a);
	DATA_PAYMENTADDRESSES(pb, b);
	cmp_t c = CMP_BIGINT(pa->userid, pb->userid);
	if (c == 0) {
		c = CMP_TV(pb->expirydate, pa->expirydate);
		if (c == 0)
			c = CMP_STR(pa->payaddress, pb->payaddress);
	}
	return c;
}

K_ITEM *find_paymentaddresses(int64_t userid)
{
	PAYMENTADDRESSES paymentaddresses, *pa;
	K_TREE_CTX ctx[1];
	K_ITEM look, *item;

	paymentaddresses.userid = userid;
	paymentaddresses.payaddress[0] = '\0';
	paymentaddresses.expirydate.tv_sec = DATE_S_EOT;

	INIT_PAYMENTADDRESSES(&look);
	look.data = (void *)(&paymentaddresses);
	item = find_after_in_ktree(paymentaddresses_root, &look, cmp_paymentaddresses, ctx);
	if (item) {
		DATA_PAYMENTADDRESSES(pa, item);
		if (pa->userid == userid && CURRENT(&(pa->expirydate)))
			return item;
		else
			return NULL;
	} else
		return NULL;
}

// order by userid asc,paydate asc,payaddress asc,expirydate desc
cmp_t cmp_payments(K_ITEM *a, K_ITEM *b)
{
	PAYMENTS *pa, *pb;
	DATA_PAYMENTS(pa, a);
	DATA_PAYMENTS(pb, b);
	cmp_t c = CMP_BIGINT(pa->userid, pb->userid);
	if (c == 0) {
		c = CMP_TV(pa->paydate, pb->paydate);
		if (c == 0) {
			c = CMP_STR(pa->payaddress, pb->payaddress);
			if (c == 0)
				c = CMP_TV(pb->expirydate, pa->expirydate);
		}
	}
	return c;
}

// order by optionname asc,activationdate asc,activationheight asc,expirydate desc
cmp_t cmp_optioncontrol(K_ITEM *a, K_ITEM *b)
{
	OPTIONCONTROL *oca, *ocb;
	DATA_OPTIONCONTROL(oca, a);
	DATA_OPTIONCONTROL(ocb, b);
	cmp_t c = CMP_STR(oca->optionname, ocb->optionname);
	if (c == 0) {
		c = CMP_TV(oca->activationdate, ocb->activationdate);
		if (c == 0) {
			c = CMP_INT(oca->activationheight, ocb->activationheight);
			if (c == 0)
				c = CMP_TV(ocb->expirydate, oca->expirydate);
		}
	}
	return c;
}

// Must be R or W locked before call
K_ITEM *find_optioncontrol(char *optionname, tv_t *now)
{
	OPTIONCONTROL optioncontrol, *oc, *ocbest;
	K_TREE_CTX ctx[1];
	K_ITEM look, *item, *best;

	/* Step through all records having optionaname and check:
	 * 1) activationdate is <= now
	 *  and
	 * 2) height <= current
	 * Remember the active record with the newest activationdate
	 * If two records have the same activation date, then
	 *  remember the active record with the highest height
	 * In optioncontrol_add(), when not specified,
	 *  the default activation date is DATE_BEGIN
	 *  and the default height is 1 (OPTIONCONTROL_HEIGHT)
	 * Thus if records have both values set, then
	 *  activationdate will determine the newests record
	 * To have activationheight decide selection,
	 *  create all records with only activationheight and then
	 *  activationdate will all be the default value and not
	 *  decide the outcome */
	STRNCPY(optioncontrol.optionname, optionname);
	optioncontrol.activationdate.tv_sec = 0L;
	optioncontrol.activationdate.tv_usec = 0L;
	optioncontrol.activationheight = OPTIONCONTROL_HEIGHT - 1;
	optioncontrol.expirydate.tv_sec = default_expiry.tv_sec;
	optioncontrol.expirydate.tv_usec = default_expiry.tv_usec;

	INIT_OPTIONCONTROL(&look);
	look.data = (void *)(&optioncontrol);
	item = find_after_in_ktree(optioncontrol_root, &look, cmp_optioncontrol, ctx);
	ocbest = NULL;
	best = NULL;
	while (item) {
		DATA_OPTIONCONTROL(oc, item);
		// Ordered first by optionname
		if (strcmp(oc->optionname, optionname) != 0)
			break;

		// Is oc active?
		if (CURRENT(&(oc->expirydate)) &&
		    oc->activationheight <= pool.height &&
		    tv_newer_eq(&(oc->activationdate), now)) {
			// Is oc newer than ocbest?
			if (!ocbest ||
			    tv_newer(&(ocbest->activationdate), &(oc->activationdate)) ||
			    (tv_equal(&(ocbest->activationdate), &(oc->activationdate)) &&
			     ocbest->activationheight < oc->activationheight)) {
				ocbest = oc;
				best = item;
			}
		}
		item = next_in_ktree(ctx);
	}
	return best;
}

// order by workinfoid asc,expirydate asc
cmp_t cmp_workinfo(K_ITEM *a, K_ITEM *b)
{
	WORKINFO *wa, *wb;
	DATA_WORKINFO(wa, a);
	DATA_WORKINFO(wb, b);
	cmp_t c = CMP_BIGINT(wa->workinfoid, wb->workinfoid);
	if (c == 0)
		c = CMP_TV(wa->expirydate, wb->expirydate);
	return c;
}

inline int32_t _coinbase1height(char *coinbase1, WHERE_FFL_ARGS)
{
	int32_t height = 0;
	uchar *cb1;
	int siz;

	cb1 = ((uchar *)coinbase1) + 84;
	siz = ((hex2bin_tbl[*cb1]) << 4) + (hex2bin_tbl[*(cb1+1)]);

	// limit to 4 for int32_t and since ... that should last a while :)
	if (siz < 1 || siz > 4) {
		LOGERR("%s(): Invalid coinbase1 block height size (%d)"
			" require: 1..4 (cb1 %s)" WHERE_FFL,
			__func__, siz, coinbase1, WHERE_FFL_PASS);
		return height;
	}

	siz *= 2;
	while (siz-- > 0) {
		height <<= 4;
		height += (int32_t)hex2bin_tbl[*(cb1+(siz^1)+2)];
	}

	return height;
}

cmp_t _cmp_height(char *coinbase1a, char *coinbase1b, WHERE_FFL_ARGS)
{
	return CMP_INT(_coinbase1height(coinbase1a, WHERE_FFL_PASS),
		       _coinbase1height(coinbase1b, WHERE_FFL_PASS));
}

// order by height asc,createdate asc
cmp_t cmp_workinfo_height(K_ITEM *a, K_ITEM *b)
{
	WORKINFO *wa, *wb;
	DATA_WORKINFO(wa, a);
	DATA_WORKINFO(wb, b);
	cmp_t c = cmp_height(wa->coinbase1, wb->coinbase1);
	if (c == 0)
		c = CMP_TV(wa->createdate, wb->createdate);
	return c;
}

K_ITEM *find_workinfo(int64_t workinfoid)
{
	WORKINFO workinfo;
	K_TREE_CTX ctx[1];
	K_ITEM look, *item;

	workinfo.workinfoid = workinfoid;
	workinfo.expirydate.tv_sec = default_expiry.tv_sec;
	workinfo.expirydate.tv_usec = default_expiry.tv_usec;

	INIT_WORKINFO(&look);
	look.data = (void *)(&workinfo);
	K_RLOCK(workinfo_free);
	item = find_in_ktree(workinfo_root, &look, cmp_workinfo, ctx);
	K_RUNLOCK(workinfo_free);
	return item;
}

// order by workinfoid asc,userid asc,workername asc,createdate asc,nonce asc,expirydate desc
cmp_t cmp_shares(K_ITEM *a, K_ITEM *b)
{
	SHARES *sa, *sb;
	DATA_SHARES(sa, a);
	DATA_SHARES(sb, b);
	cmp_t c = CMP_BIGINT(sa->workinfoid, sb->workinfoid);
	if (c == 0) {
		c = CMP_BIGINT(sa->userid, sb->userid);
		if (c == 0) {
			c = CMP_STR(sa->workername, sb->workername);
			if (c == 0) {
				c = CMP_TV(sa->createdate, sb->createdate);
				if (c == 0) {
					c = CMP_STR(sa->nonce, sb->nonce);
					if (c == 0) {
						c = CMP_TV(sb->expirydate,
							   sa->expirydate);
					}
				}
			}
		}
	}
	return c;
}

// order by workinfoid asc,userid asc,createdate asc,nonce asc,expirydate desc
cmp_t cmp_shareerrors(K_ITEM *a, K_ITEM *b)
{
	SHAREERRORS *sa, *sb;
	DATA_SHAREERRORS(sa, a);
	DATA_SHAREERRORS(sb, b);
	cmp_t c = CMP_BIGINT(sa->workinfoid, sb->workinfoid);
	if (c == 0) {
		c = CMP_BIGINT(sa->userid, sb->userid);
		if (c == 0) {
			c = CMP_TV(sa->createdate, sb->createdate);
			if (c == 0)
				c = CMP_TV(sb->expirydate, sa->expirydate);
		}
	}
	return c;
}

void dsp_sharesummary(K_ITEM *item, FILE *stream)
{
	char createdate_buf[DATE_BUFSIZ];
	SHARESUMMARY *s;

	if (!item)
		fprintf(stream, "%s() called with (null) item\n", __func__);
	else {
		DATA_SHARESUMMARY(s, item);

		tv_to_buf(&(s->createdate), createdate_buf, sizeof(createdate_buf));
		fprintf(stream, " uid=%"PRId64" wn='%s' wid=%"PRId64" "
				"da=%f ds=%f ss=%f c='%s' cd=%s\n",
				s->userid, s->workername, s->workinfoid,
				s->diffacc, s->diffsta, s->sharesta,
				s->complete, createdate_buf);
	}
}

// default tree order by userid asc,workername asc,workinfoid asc for reporting
cmp_t cmp_sharesummary(K_ITEM *a, K_ITEM *b)
{
	SHARESUMMARY *sa, *sb;
	DATA_SHARESUMMARY(sa, a);
	DATA_SHARESUMMARY(sb, b);
	cmp_t c = CMP_BIGINT(sa->userid, sb->userid);
	if (c == 0) {
		c = CMP_STR(sa->workername, sb->workername);
		if (c == 0)
			c = CMP_BIGINT(sa->workinfoid, sb->workinfoid);
	}
	return c;
}

// order by workinfoid asc,userid asc,workername asc for flagging complete
cmp_t cmp_sharesummary_workinfoid(K_ITEM *a, K_ITEM *b)
{
	SHARESUMMARY *sa, *sb;
	DATA_SHARESUMMARY(sa, a);
	DATA_SHARESUMMARY(sb, b);
	cmp_t c = CMP_BIGINT(sa->workinfoid, sb->workinfoid);
	if (c == 0) {
		c = CMP_BIGINT(sa->userid, sb->userid);
		if (c == 0)
			c = CMP_STR(sa->workername, sb->workername);
	}
	return c;
}

K_ITEM *find_sharesummary(int64_t userid, char *workername, int64_t workinfoid)
{
	SHARESUMMARY sharesummary;
	K_TREE_CTX ctx[1];
	K_ITEM look;

	sharesummary.userid = userid;
	STRNCPY(sharesummary.workername, workername);
	sharesummary.workinfoid = workinfoid;

	INIT_SHARESUMMARY(&look);
	look.data = (void *)(&sharesummary);
	return find_in_ktree(sharesummary_root, &look, cmp_sharesummary, ctx);
}

void dsp_blocks(K_ITEM *item, FILE *stream)
{
	char createdate_buf[DATE_BUFSIZ], expirydate_buf[DATE_BUFSIZ];
	BLOCKS *b = NULL;
	char hash_dsp[16+1];

	if (!item)
		fprintf(stream, "%s() called with (null) item\n", __func__);
	else {
		DATA_BLOCKS(b, item);

		dsp_hash(b->blockhash, hash_dsp, sizeof(hash_dsp));
		tv_to_buf(&(b->createdate), createdate_buf, sizeof(createdate_buf));
		tv_to_buf(&(b->expirydate), expirydate_buf, sizeof(expirydate_buf));
		fprintf(stream, " hi=%d hash='%.16s' conf=%s uid=%"PRId64
				" w='%s' sconf=%s cd=%s ed=%s\n",
				b->height, hash_dsp, b->confirmed, b->userid,
				b->workername, b->statsconfirmed,
				createdate_buf, expirydate_buf);
	}
}

// order by height asc,blockhash asc,expirydate desc
cmp_t cmp_blocks(K_ITEM *a, K_ITEM *b)
{
	BLOCKS *ba, *bb;
	DATA_BLOCKS(ba, a);
	DATA_BLOCKS(bb, b);
	cmp_t c = CMP_INT(ba->height, bb->height);
	if (c == 0) {
		c = CMP_STR(ba->blockhash, bb->blockhash);
		if (c == 0)
			c = CMP_TV(bb->expirydate, ba->expirydate);
	}
	return c;
}

/* TODO: and make sure all block searches use these
 * or add new ones as required here */

// Must be R or W locked before call - gets current status (default_expiry)
K_ITEM *find_blocks(int32_t height, char *blockhash)
{
	BLOCKS blocks;
	K_TREE_CTX ctx[1];
	K_ITEM look;

	blocks.height = height;
	STRNCPY(blocks.blockhash, blockhash);
	blocks.expirydate.tv_sec = default_expiry.tv_sec;
	blocks.expirydate.tv_usec = default_expiry.tv_usec;

	INIT_BLOCKS(&look);
	look.data = (void *)(&blocks);
	return find_in_ktree(blocks_root, &look, cmp_blocks, ctx);
}

// Must be R or W locked before call
K_ITEM *find_prev_blocks(int32_t height)
{
	BLOCKS lookblocks, *blocks;
	K_TREE_CTX ctx[1];
	K_ITEM look, *b_item;

	/* TODO: For self orphaned (if that ever happens)
	 * this will find based on blockhash order if it has two,
	 * not NEW, blocks, which might not find the right one */
	lookblocks.height = height;
	lookblocks.blockhash[0] = '\0';
	lookblocks.expirydate.tv_sec = 0L;
	lookblocks.expirydate.tv_usec = 0L;

	INIT_BLOCKS(&look);
	look.data = (void *)(&lookblocks);
	b_item = find_before_in_ktree(blocks_root, &look, cmp_blocks, ctx);
	while (b_item) {
		DATA_BLOCKS(blocks, b_item);
		if (blocks->confirmed[0] != BLOCKS_NEW &&
		    CURRENT(&(blocks->expirydate)))
			return b_item;
		b_item = prev_in_ktree(ctx);
	}
	return NULL;
}

const char *blocks_confirmed(char *confirmed)
{
	switch (confirmed[0]) {
		case BLOCKS_NEW:
			return blocks_new;
		case BLOCKS_CONFIRM:
			return blocks_confirm;
		case BLOCKS_42:
			return blocks_42;
		case BLOCKS_ORPHAN:
			return blocks_orphan;
	}
	return blocks_unknown;
}

/* order by height asc,userid asc,expirydate asc
 * i.e. only one payout amount per block per user */
cmp_t cmp_miningpayouts(K_ITEM *a, K_ITEM *b)
{
	MININGPAYOUTS *ma, *mb;
	DATA_MININGPAYOUTS(ma, a);
	DATA_MININGPAYOUTS(mb, b);
	cmp_t c = CMP_INT(ma->height, mb->height);
	if (c == 0) {
		c = CMP_BIGINT(ma->userid, mb->userid);
		if (c == 0)
			c = CMP_TV(ma->expirydate, mb->expirydate);
	}
	return c;
}

// order by userid asc,createdate asc,authid asc,expirydate desc
cmp_t cmp_auths(K_ITEM *a, K_ITEM *b)
{
	AUTHS *aa, *ab;
	DATA_AUTHS(aa, a);
	DATA_AUTHS(ab, b);
	cmp_t c = CMP_BIGINT(aa->userid, ab->userid);
	if (c == 0) {
		c = CMP_TV(aa->createdate, ab->createdate);
		if (c == 0) {
			c = CMP_BIGINT(aa->authid, ab->authid);
			if (c == 0)
				c = CMP_TV(ab->expirydate, aa->expirydate);
		}
	}
	return c;
}

// order by poolinstance asc,createdate asc
cmp_t cmp_poolstats(K_ITEM *a, K_ITEM *b)
{
	POOLSTATS *pa, *pb;
	DATA_POOLSTATS(pa, a);
	DATA_POOLSTATS(pb, b);
	cmp_t c = CMP_STR(pa->poolinstance, pb->poolinstance);
	if (c == 0)
		c = CMP_TV(pa->createdate, pb->createdate);
	return c;
}

void dsp_userstats(K_ITEM *item, FILE *stream)
{
	char statsdate_buf[DATE_BUFSIZ], createdate_buf[DATE_BUFSIZ];
	USERSTATS *u = NULL;

	if (!item)
		fprintf(stream, "%s() called with (null) item\n", __func__);
	else {
		DATA_USERSTATS(u, item);
		tv_to_buf(&(u->statsdate), statsdate_buf, sizeof(statsdate_buf));
		tv_to_buf(&(u->createdate), createdate_buf, sizeof(createdate_buf));
		fprintf(stream, " pi='%s' uid=%"PRId64" w='%s' e=%"PRId64" Hs=%f "
				"Hs5m=%f Hs1hr=%f Hs24hr=%f sl=%s sc=%d sd=%s cd=%s\n",
				u->poolinstance, u->userid, u->workername,
				u->elapsed, u->hashrate, u->hashrate5m,
				u->hashrate1hr, u->hashrate24hr, u->summarylevel,
				u->summarycount, statsdate_buf, createdate_buf);
	}
}

/* order by userid asc,statsdate asc,poolinstance asc,workername asc
   as per required for userstats homepage summarisation */
cmp_t cmp_userstats(K_ITEM *a, K_ITEM *b)
{
	USERSTATS *ua, *ub;
	DATA_USERSTATS(ua, a);
	DATA_USERSTATS(ub, b);
	cmp_t c = CMP_BIGINT(ua->userid, ub->userid);
	if (c == 0) {
		c = CMP_TV(ua->statsdate, ub->statsdate);
		if (c == 0) {
			c = CMP_STR(ua->poolinstance, ub->poolinstance);
			if (c == 0)
				c = CMP_STR(ua->workername, ub->workername);
		}
	}
	return c;
}

/* order by userid asc,workername asc
   temporary tree for summing userstats when sending user homepage info */
cmp_t cmp_userstats_workername(K_ITEM *a, K_ITEM *b)
{
	USERSTATS *ua, *ub;
	DATA_USERSTATS(ua, a);
	DATA_USERSTATS(ub, b);
	cmp_t c = CMP_BIGINT(ua->userid, ub->userid);
	if (c == 0)
		c = CMP_STR(ua->workername, ub->workername);
	return c;
}

/* order by statsdate,userid asc,statsdate asc,workername asc,poolinstance asc
   as per required for DB summarisation */
cmp_t cmp_userstats_statsdate(K_ITEM *a, K_ITEM *b)
{
	USERSTATS *ua, *ub;
	DATA_USERSTATS(ua, a);
	DATA_USERSTATS(ub, b);
	cmp_t c = CMP_TV(ua->statsdate, ub->statsdate);
	if (c == 0) {
		c = CMP_BIGINT(ua->userid, ub->userid);
		if (c == 0) {
			c = CMP_STR(ua->workername, ub->workername);
			if (c == 0)
				c = CMP_STR(ua->poolinstance, ub->poolinstance);
		}
	}
	return c;
}

/* order by userid asc,workername asc,statsdate asc,poolinstance asc
   built during data load to update workerstatus at the end of the load
   and used during reload to discard stats already in the DB */
cmp_t cmp_userstats_workerstatus(K_ITEM *a, K_ITEM *b)
{
	USERSTATS *ua, *ub;
	DATA_USERSTATS(ua, a);
	DATA_USERSTATS(ub, b);
	cmp_t c = CMP_BIGINT(ua->userid, ub->userid);
	if (c == 0) {
		c = CMP_STR(ua->workername, ub->workername);
		if (c == 0) {
			c = CMP_TV(ua->statsdate, ub->statsdate);
			if (c == 0)
				c = CMP_STR(ua->poolinstance, ub->poolinstance);
		}
	}
	return c;
}