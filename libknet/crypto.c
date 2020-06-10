/*
 * Copyright (C) 2012-2020 Red Hat, Inc.  All rights reserved.
 *
 * Author: Fabio M. Di Nitto <fabbione@kronosnet.org>
 *
 * This software licensed under LGPL-2.0+
 */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "crypto.h"
#include "crypto_model.h"
#include "internals.h"
#include "logging.h"
#include "common.h"

/*
 * internal module switch data
 */

static crypto_model_t crypto_modules_cmds[] = {
	{ "nss", WITH_CRYPTO_NSS, 0, NULL },
	{ "openssl", WITH_CRYPTO_OPENSSL, 0, NULL },
	{ NULL, 0, 0, NULL }
};

static int crypto_get_model(const char *model)
{
	int idx = 0;

	while (crypto_modules_cmds[idx].model_name != NULL) {
		if (!strcmp(crypto_modules_cmds[idx].model_name, model))
			return idx;
		idx++;
	}
	return -1;
}

/*
 * exported API
 */

int crypto_encrypt_and_sign (
	knet_handle_t knet_h,
	const unsigned char *buf_in,
	const ssize_t buf_in_len,
	unsigned char *buf_out,
	ssize_t *buf_out_len)
{
	return crypto_modules_cmds[knet_h->crypto_instance[knet_h->crypto_in_use_config]->model].ops->crypt(knet_h, knet_h->crypto_instance[knet_h->crypto_in_use_config], buf_in, buf_in_len, buf_out, buf_out_len);
}

int crypto_encrypt_and_signv (
	knet_handle_t knet_h,
	const struct iovec *iov_in,
	int iovcnt_in,
	unsigned char *buf_out,
	ssize_t *buf_out_len)
{
	return crypto_modules_cmds[knet_h->crypto_instance[knet_h->crypto_in_use_config]->model].ops->cryptv(knet_h, knet_h->crypto_instance[knet_h->crypto_in_use_config], iov_in, iovcnt_in, buf_out, buf_out_len);
}

int crypto_authenticate_and_decrypt (
	knet_handle_t knet_h,
	const unsigned char *buf_in,
	const ssize_t buf_in_len,
	unsigned char *buf_out,
	ssize_t *buf_out_len)
{
	int i, err = 0;
	int multiple_configs = 0;
	uint8_t log_level = KNET_LOG_ERR;

	for (i = 1; i <= KNET_MAX_CRYPTO_INSTANCES; i++) {
		if (knet_h->crypto_instance[i]) {
			multiple_configs++;
		}
	}

	/*
	 * attempt to decrypt first with the in-use config
	 * to avoid excessive performance hit.
	 */

	if (multiple_configs > 1) {
		log_level = KNET_LOG_DEBUG;
	}

	err = crypto_modules_cmds[knet_h->crypto_instance[knet_h->crypto_in_use_config]->model].ops->decrypt(knet_h, knet_h->crypto_instance[knet_h->crypto_in_use_config], buf_in, buf_in_len, buf_out, buf_out_len, log_level);

	/*
	 * if we fail, try to use the other configurations
	 */
	if (err) {
		for (i = 1; i <= KNET_MAX_CRYPTO_INSTANCES; i++) {
			/*
			 * in-use config was already attempted
			 */
			if (i == knet_h->crypto_in_use_config) {
				continue;
			}
			if (knet_h->crypto_instance[i]) {
				log_debug(knet_h, KNET_SUB_CRYPTO, "Alternative crypto configuration found, attempting to decrypt with config %u", i);
				err = crypto_modules_cmds[knet_h->crypto_instance[i]->model].ops->decrypt(knet_h, knet_h->crypto_instance[i], buf_in, buf_in_len, buf_out, buf_out_len, KNET_LOG_ERR);
				if (!err) {
					errno = 0; /* clear errno from previous failures */
					return err;
				}
				log_debug(knet_h, KNET_SUB_CRYPTO, "Packet failed to decrypt with crypto config %u", i);
			}
		}
	}
	return err;
}

int crypto_use_config(
	knet_handle_t knet_h,
	uint8_t config_num)
{
	if (!knet_h->crypto_instance[config_num]) {
		errno = EINVAL;
		return -1;
	}

	knet_h->crypto_in_use_config = config_num;
	knet_h->sec_block_size = knet_h->crypto_instance[config_num]->sec_block_size;
	knet_h->sec_hash_size = knet_h->crypto_instance[config_num]->sec_hash_size;
	knet_h->sec_salt_size = knet_h->crypto_instance[config_num]->sec_salt_size;

	return 0;
}

int crypto_init(
	knet_handle_t knet_h,
	struct knet_handle_crypto_cfg *knet_handle_crypto_cfg,
	uint8_t config_num)
{
	int err = 0, savederrno = 0;
	int model = 0;
	struct crypto_instance *current = NULL, *new = NULL;

	current = knet_h->crypto_instance[config_num];

	model = crypto_get_model(knet_handle_crypto_cfg->crypto_model);
	if (model < 0) {
		log_err(knet_h, KNET_SUB_CRYPTO, "model %s not supported", knet_handle_crypto_cfg->crypto_model);
		return -1;
	}

	if (crypto_modules_cmds[model].built_in == 0) {
		log_err(knet_h, KNET_SUB_CRYPTO, "this version of libknet was built without %s support. Please contact your vendor or fix the build.", knet_handle_crypto_cfg->crypto_model);
		return -1;
	}

	savederrno = pthread_rwlock_wrlock(&shlib_rwlock);
	if (savederrno) {
		log_err(knet_h, KNET_SUB_CRYPTO, "Unable to get write lock: %s",
			strerror(savederrno));
		return -1;
	}

	if (!crypto_modules_cmds[model].loaded) {
		crypto_modules_cmds[model].ops = load_module (knet_h, "crypto", crypto_modules_cmds[model].model_name);
		if (!crypto_modules_cmds[model].ops) {
			savederrno = errno;
			err = -1;
			log_err(knet_h, KNET_SUB_CRYPTO, "Unable to load %s lib", crypto_modules_cmds[model].model_name);
			goto out;
		}
		if (crypto_modules_cmds[model].ops->abi_ver != KNET_CRYPTO_MODEL_ABI) {
			savederrno = EINVAL;
			err = -1;
			log_err(knet_h, KNET_SUB_CRYPTO,
				"ABI mismatch loading module %s. knet ver: %d, module ver: %d",
				crypto_modules_cmds[model].model_name, KNET_CRYPTO_MODEL_ABI,
				crypto_modules_cmds[model].ops->abi_ver);
			goto out;
		}
		crypto_modules_cmds[model].loaded = 1;
	}

	log_debug(knet_h, KNET_SUB_CRYPTO,
		  "Initizializing crypto module [%s/%s/%s]",
		  knet_handle_crypto_cfg->crypto_model,
		  knet_handle_crypto_cfg->crypto_cipher_type,
		  knet_handle_crypto_cfg->crypto_hash_type);

	new = malloc(sizeof(struct crypto_instance));

	if (!new) {
		savederrno = ENOMEM;
		err = -1;
		log_err(knet_h, KNET_SUB_CRYPTO, "Unable to allocate memory for crypto instance");
		goto out;
	}

	/*
	 * if crypto_modules_cmds.ops->init fails, it is expected that
	 * it will clean everything by itself.
	 * crypto_modules_cmds.ops->fini is not invoked on error.
	 */
	new->model = model;
	if (crypto_modules_cmds[model].ops->init(knet_h, new, knet_handle_crypto_cfg)) {
		savederrno = errno;
		err = -1;
		goto out;
	}

out:
	if (!err) {
		knet_h->crypto_instance[config_num] = new;
		if ((!knet_h->crypto_in_use_config) || (knet_h->crypto_in_use_config == config_num)) {
			knet_h->sec_block_size = new->sec_block_size;
			knet_h->sec_hash_size = new->sec_hash_size;
			knet_h->sec_salt_size = new->sec_salt_size;
		}
		/*
		 * set default config only at first crypto init
		 */
		if (!knet_h->crypto_in_use_config) {
			knet_h->crypto_in_use_config = config_num;
		}

		log_debug(knet_h, KNET_SUB_CRYPTO, "Hash size: %zu salt size: %zu block size: %zu",
			  knet_h->sec_hash_size,
			  knet_h->sec_salt_size,
			  knet_h->sec_block_size);

		if (current) {
			if (crypto_modules_cmds[current->model].ops->fini != NULL) {
				crypto_modules_cmds[current->model].ops->fini(knet_h, current);
			}
			free(current);
		}
	} else {
		if (new) {
			free(new);
		}
	}

	pthread_rwlock_unlock(&shlib_rwlock);
	errno = err ? savederrno : 0;
	return err;
}

static void crypto_fini_config(
	knet_handle_t knet_h,
	uint8_t config_num)
{
	if (knet_h->crypto_instance[config_num]) {
		if (crypto_modules_cmds[knet_h->crypto_instance[config_num]->model].ops->fini != NULL) {
			crypto_modules_cmds[knet_h->crypto_instance[config_num]->model].ops->fini(knet_h, knet_h->crypto_instance[config_num]);
		}
		free(knet_h->crypto_instance[config_num]);
		knet_h->crypto_instance[config_num] = NULL;
		if (knet_h->crypto_in_use_config == config_num) {
			knet_h->crypto_in_use_config = 0;
			knet_h->sec_block_size = 0;
			knet_h->sec_hash_size = 0;
			knet_h->sec_salt_size = 0;
		}
	}
}

void crypto_fini(
	knet_handle_t knet_h,
	uint8_t config_num)
{
	int savederrno = 0, i;

	savederrno = pthread_rwlock_wrlock(&shlib_rwlock);
	if (savederrno) {
		log_err(knet_h, KNET_SUB_CRYPTO, "Unable to get write lock: %s",
			strerror(savederrno));
		return;
	}

	if (!config_num) {
		for (i = 1; i <= KNET_MAX_CRYPTO_INSTANCES; i++) {
			crypto_fini_config(knet_h, i);
		}
		knet_h->crypto_in_use_config = 0;
	} else {
		crypto_fini_config(knet_h, config_num);
		if (knet_h->crypto_in_use_config == config_num) {
			knet_h->crypto_in_use_config = 0;
		}
	}

	pthread_rwlock_unlock(&shlib_rwlock);
	return;
}

int knet_get_crypto_list(struct knet_crypto_info *crypto_list, size_t *crypto_list_entries)
{
	int err = 0;
	int idx = 0;
	int outidx = 0;

	if (!crypto_list_entries) {
		errno = EINVAL;
		return -1;
	}

	while (crypto_modules_cmds[idx].model_name != NULL) {
		if (crypto_modules_cmds[idx].built_in) {
			if (crypto_list) {
				crypto_list[outidx].name = crypto_modules_cmds[idx].model_name;
			}
			outidx++;
		}
		idx++;
	}
	*crypto_list_entries = outidx;

	if (!err)
		errno = 0;
	return err;
}
