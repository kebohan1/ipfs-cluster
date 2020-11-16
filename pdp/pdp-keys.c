
/* 
* pdp-keys.c
*
* Copyright (c) 2008, Zachary N J Peterson <zachary@jhu.edu>
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in the
*       documentation and/or other materials provided with the distribution.
*     * The name of the Zachary N J Peterson may be used to endorse or promote products
*       derived from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY ZACHARY N J PETERSON ``AS IS'' AND ANY
* EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL ZACHARY N J PETERSON BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "pdp.h"
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/aes.h>
#include <pwd.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <paths.h>
#include <stdio.h>

/* Define some paths for storing keys */
#define PATH_PDP_USER_DIR ".pdp"
#define PATH_PDP_PRIVATE_KEY ".pdp/pdp.pri"
#define PATH_PDP_PUBLIC_KEY ".pdp/pdp.pub"



/* nist_key_wrap: Performs the NIST AES Key Wrap used to securely and authentically encrypt a key for storage
* on an unstrusted medium, e.g. disk.
* It takes in the key to be encrypted and its size in bytes (a minimum of 128 bits) and the key-encryption-key (kek) and
* its size in bytes.  The kek must be 128, 192 or 256 bits.
* Returns an allocted buffer containing the encrypted key, which will be key_size + 8 bytes in size.
*/
static unsigned char *nist_key_wrap(unsigned char *key, size_t key_size, unsigned char *kek, size_t kek_size){

	AES_KEY aes_key;
	unsigned char A[8];
	unsigned int i = 0;
	unsigned int j = 0;
	unsigned int n = 0;
	unsigned int t = 0;
	unsigned char *r_array = NULL; /* Wrap registers */ 
	unsigned char *c_array = NULL; /* Ciphertext */
	unsigned char aes_input[PRP_KEY_SIZE];
	unsigned char aes_output[PRP_KEY_SIZE];

	if(!key || !kek) return NULL;

	/* Set the A to the magic number */
	memset(A, 0xA6, 8);
	memset(&aes_key, 0, sizeof(AES_KEY));
	memset(aes_input, 0, PRP_KEY_SIZE);
	memset(aes_output, 0, PRP_KEY_SIZE);
		
	/* set n - the number of 64 bit values in key*/
	n = (key_size/8);

	/* Set up the R array - n 64 bit blocks */
	if( ((r_array = malloc(8 * n)) == NULL)) return NULL;
	memcpy(r_array, key, (n*8));

	/* Setup the AES key */	
	if(AES_set_encrypt_key(kek, kek_size * 8, &aes_key) != 0) goto cleanup;
		
	for(j = 0; j < 6; j++){
		for(i = 0; i < n; i++){

			/* Copy A into the first 64 bits of the input */
			memcpy(aes_input, A, 8);
			
			/* Copy R_i into the second 64 bits of the input */
			memcpy((aes_input + 8), (r_array + (i*8)), 8);
			
			/* AES(A | R_i) */
			AES_encrypt(aes_input, aes_output, &aes_key);

			/* Get the 64 most significant bits from the output and put them in A*/
			memcpy(A, aes_output, 8);
			
			/* XOR A and t, where t = (n * j) + i */
			t = ((n * j) + (i + 1));
			A[7] = A[7] ^ t;
			
			/* R_i gets the least significat 64 bits of the aes_output */
			memcpy((r_array + (i*8)), (aes_output + 8), 8);		
		}
	}
	
	/* Set up the C array - n + 1 64 bit blocks */
	if( ((c_array = malloc(8 * (n + 1))) == NULL)) goto cleanup;
	/* C_0 gets A */
	memcpy(c_array, A, 8);
	/* C_i gets R_i */
	memcpy((c_array + 8), r_array, (8 * n));
	
	if(r_array) sfree(r_array, 8*n);
	
	return c_array;
	
cleanup:
	if(r_array) sfree(r_array, 8*n);
	if(c_array) sfree(c_array, (8*(n+1)));
	
	return NULL;
}


/* nist_key_unwrap: Performs the NIST AES Key Wrap unwraping function used to securely and authentically decrypt a key 
* that has been wrapped.
* It takes in the encrypted key to be decrypted and its size in bytes (which will be original key size plus 8 bytes)
* and the key-encryption-key (kek) and its size in bytes.  The kek must be 128, 192 or 256 bits.
* Returns an allocted buffer containing the decrypted key, which will be (enckey_size - 8) bytes in size.
*/
static unsigned char *nist_key_unwrap(unsigned char *enckey, size_t enckey_size, unsigned char *kek, size_t kek_size){

	AES_KEY aes_key;
	unsigned char A[8];
	unsigned char A_check[8];
	int i = 0;
	int j = 0;
	unsigned int n = 0;
	unsigned int t = 0;
	unsigned char *r_array = NULL; /* Wrap registers */
	unsigned char *p_array = NULL; /* Plaintext */
	unsigned char aes_input[PRP_KEY_SIZE];
	unsigned char aes_output[PRP_KEY_SIZE];

	if(!enckey || !kek) return NULL;

	/* Set the A_check to the magic number */
	memset(A_check, 0xA6, 8);
	memset(&aes_key, 0, sizeof(AES_KEY));
	memset(aes_input, 0, PRP_KEY_SIZE);
	memset(aes_output, 0, PRP_KEY_SIZE);
		
	/* set n - the number of 64 bit values in key*/
	n = (enckey_size/8) - 1;

	/* Set up the R array - n 64 bit blocks */
	if( ((r_array = malloc(8 * n)) == NULL)) return NULL;	

	/* Setup the AES key */
	if(AES_set_decrypt_key(kek, kek_size * 8, &aes_key) != 0) return NULL;

	/* Initialize A and the R array */
	memcpy(A, enckey, 8);
	memcpy(r_array, enckey + 8, n*8);
		
	for(j = 5; j >= 0; j--){
		for(i = (n-1); i >= 0; i--){
			
			/* XOR A and t, where t = (n * j) + i */
			t = ((n * j) + (i + 1));
			A[7] = A[7] ^ t;
			
			/* Copy A XOR t into the first 64 bits of the input */
			memcpy(aes_input, A, 8);
			
			/* Copy R_i into the second 64 bits of the input */
			memcpy((aes_input + 8), (r_array + (i*8)), 8);
			
			/* AES-1(A | R_i) */
			AES_decrypt(aes_input, aes_output, &aes_key);
			
			/* Get the 64 most significant bits from the output and put them in A*/
			memcpy(A, aes_output, 8);
			
			/* R_i gets the least significat 64 bits of the aes_output */
			memcpy((r_array + (i*8)), (aes_output + 8), 8);		
		}
	}
	
	/* Perform an integrity check on A */
	if(memcmp(A, A_check, 8) != 0) goto cleanup;
	
	/* Set up the P array - n 64 bit blocks */
	if( ((p_array = malloc(8 * n)) == NULL)) goto cleanup;
	/* P_i gets R_i */
	memcpy(p_array, r_array, (8 * n));
	
	if(r_array) sfree(r_array, 8*n);
		
	return p_array;
	
cleanup:
	if(r_array) sfree(r_array, 8*n);
	if(p_array) sfree(p_array, (8*n));
		
	return NULL;
}

/* read_password: Display the prompt and read a password off the terminal with echo off into the
*  buffer, buf.  Return a pointer to the password or NULL on failure.  */
static unsigned char *read_password(const char *prompt, unsigned char *buf, size_t bufsize){

	FILE *term = NULL;
	int termfd = -1;
	sigset_t saved_signals;
	sigset_t set_signals;
	unsigned char *password = NULL;
	struct termios saved_term;
	struct termios set_term;
	char ch = 0;
	int ctr = 0;
	
	if(buf)
		password = buf;
	else{
		if( ((password = malloc(1024)) == NULL)) goto cleanup;
		bufsize = 1024;
		memset(password, 0, 1024);
	}
	
	/* Open the terminal */
	term = fopen(_PATH_TTY, "r+");
	if(!term) return NULL;
	
	/* Get the file descriptor */
	termfd = fileno(term);
	
	/* Display the prompt */
	if(prompt){
		fprintf(term, "%s", prompt);
		fflush(term);
	}
	
	/* Turn off interuption */
	sigemptyset(&set_signals);
	sigaddset(&set_signals, SIGINT);
	sigaddset(&set_signals, SIGTSTP);
	sigprocmask(SIG_BLOCK, &set_signals, &saved_signals);
	
	/* Save state and turn off echo */
	tcgetattr(termfd, &saved_term);
	set_term = saved_term;
	set_term.c_lflag &= ~(ECHO|ECHOE|ECHOK|ECHONL);
	tcsetattr(termfd, TCSAFLUSH, &set_term);
	
	/* Get the password off the terminal */
	for(;;){
		if(ctr >= bufsize) goto cleanup;
		switch(read(termfd, &ch, 1)){
			case 1:
				if(ch != '\n') break;
			case 0:
				password[ctr] = '\0';
				goto done;
			default:
				goto cleanup;
		}
		password[ctr] = ch;
		ctr++;
	}

done:
	fprintf(term, "\n");

	/* Re-set the state of the term */
	tcsetattr(termfd, TCSAFLUSH, &saved_term);
	sigprocmask(SIG_SETMASK, &saved_signals, 0);
	
	if(term) fclose(term);

	return password;
	
cleanup:
	if(password && !buf) sfree(password, 1024);
	if(term) fclose(term);
	
	return NULL;
}
  

/* PBKDF2_F: A support function for PBKDF2 below.  See the PKCS5 specification for details */
static int PBKDF2_F(unsigned char *T, unsigned char *password, size_t password_len, unsigned char *salt, size_t salt_len, int c, int i){

	unsigned char U[SHA_DIGEST_LENGTH];
	unsigned char *U1 = NULL;
	unsigned int swapped_i = 0;
	unsigned int U_len = 0;
	int j = 0;
	int k = 0;
	
	if(!T || !password || !password_len || !salt || !salt_len || !c) return 0;
	
	if( ((U1 = malloc(salt_len + sizeof(unsigned int))) == NULL)) return 0;
	
	memset(U, 0, SHA_DIGEST_LENGTH);
	memset(U1, 0, salt_len + sizeof(int));
	
	/* Cat the salt and swapped byte-order of i */
	swapped_i = htonl(i);
	memcpy(U1, salt, salt_len);
	memcpy(U1 + salt_len, &swapped_i, sizeof(unsigned int));
	
	/* Perform the initial PRF, U_1 = PRF(P, S | i) */
	HMAC(EVP_sha1(), password, password_len, U1, salt_len + sizeof(unsigned int), U, &U_len);
	if(U_len != SHA_DIGEST_LENGTH) goto cleanup;
	
	for(j = 0; j < c; j++){
		/* XOR the last value of U into T, the final U value */
		for(k = 0; k < SHA_DIGEST_LENGTH; k++) T[k] ^= U[k];
		/* U_i = PRF(P, U_i-c) */
		HMAC(EVP_sha1(), password, password_len, U, SHA_DIGEST_LENGTH, U, &U_len);
		if(U_len != SHA_DIGEST_LENGTH) goto cleanup;
	}
	/* Perform the final XOR */
	for(k = 0; k < SHA_DIGEST_LENGTH; k++) T[k] ^= U[k];
	
	if(U1) sfree(U1, salt_len + sizeof(unsigned int));
	
	return 1;

cleanup:
	if(U1) sfree(U1, salt_len + sizeof(unsigned int));
	return 0;
}

/* PBKDF2: The PKCS5-based password-based key derivation function.  It takes a password, salt, iteration count and
* desired key length, and returns a password-derived key of dkey_len size or NULL on error */
static unsigned char *PBKDF2(unsigned char *password, size_t password_len, unsigned char *salt, size_t salt_len, unsigned int c, size_t dkey_len){

	unsigned int l = 0;
	unsigned int r = 0;
	unsigned char *dk = NULL;
	unsigned char T[SHA_DIGEST_LENGTH];
	size_t remaining_bytes = dkey_len;
	int i = 0;

	if(!password || !password_len || !salt || !salt_len || !c || !dkey_len) return NULL;

	if ( ((dk = malloc(dkey_len)) == NULL)) return NULL;

	memset(dk, 0, dkey_len);
	memset(T, 0, SHA_DIGEST_LENGTH);

	/* derive l, the number of SHA blocks in the derived key */
	l = (dkey_len/SHA_DIGEST_LENGTH);
	if(dkey_len%SHA_DIGEST_LENGTH) l++;
	
	/* derive r, the number of bytes in the last block */
	r = dkey_len - ((l - 1) * SHA_DIGEST_LENGTH);
	
	
	/* Compute T_i */
	for(i = 0; i < l; i++){
		
		if(!PBKDF2_F(T, password, password_len, salt, salt_len, c, i)) goto cleanup;
		
		/* Add T_i to the derived key */
		if(remaining_bytes >= SHA_DIGEST_LENGTH){
			memcpy(dk + (i * SHA_DIGEST_LENGTH), T, SHA_DIGEST_LENGTH);
			remaining_bytes -= SHA_DIGEST_LENGTH;
		}else{
			memcpy(dk + (i * SHA_DIGEST_LENGTH), T, remaining_bytes);
			remaining_bytes -= remaining_bytes;
		}
	}
	
	return dk;
	
cleanup:
	if(dk) sfree(dk, dkey_len);

	return NULL;
}


/* read_pdp_keypair: Read a PDP-keypair from a file and return a PDP_key structure.
 * Takes in two open file pointers to the private and public keys.
 * Returns an allocated PDP_key or NULL on failure.
*/
PDP_key *read_pdp_keypair(FILE *pri_key, FILE *pub_key){

	PDP_key *key = NULL;
	BN_CTX *ctx = NULL;
	BIGNUM *r1 = NULL;
	BIGNUM *r2 = NULL;
	EVP_PKEY *pkey = NULL;
	unsigned char *salt = NULL;
	unsigned char *dk = NULL; /* Derived key */
	unsigned char *enc_v = NULL;
	size_t gen_size = 0;
	unsigned char *gen = NULL;
	size_t password_len = 1024;
	char password[password_len];
	unsigned char *key_v = NULL;
	RSA *rsa = NULL;
	
	if(pri_key == NULL || pub_key == NULL) return NULL;

	if( (key = malloc(sizeof(PDP_key))) == NULL) return NULL;
	memset(key, 0, sizeof(PDP_key));
	if( ((key->g=BN_new()) == NULL)) goto cleanup;
	if( ((key->v = malloc(PRF_KEY_SIZE)) == NULL)) goto cleanup;
	if( ((r1=BN_new()) == NULL)) goto cleanup;
	if( ((r2=BN_new()) == NULL)) goto cleanup;
	if( ((ctx=BN_CTX_new()) == NULL)) goto cleanup;
	if( ((salt = malloc(PRF_KEY_SIZE)) == NULL)) goto cleanup;
	if( ((enc_v = malloc(40)) == NULL)) goto cleanup;
	
	memset(key->v, 0, PRF_KEY_SIZE);
	memset(salt, 0, PRF_KEY_SIZE);
	memset(enc_v, 0, 40);
	memset(password, 0, password_len);

#ifdef	DEBUG_MODE 
	strcpy(password, "z");
#else
	if(!read_password("Enter passphrase:", (unsigned char *)password, password_len)) goto cleanup;
	// strcpy(password,password_in);
#endif

	pkey = PEM_read_PrivateKey(pri_key, NULL, NULL, password);
	if(!pkey){ fprintf(stderr, "ERROR: Failed to decrypt key.\n"); goto cleanup;}
	key->rsa = EVP_PKEY_get1_RSA(pkey);
	if(!key->rsa) goto cleanup;
	
	if(!RSA_check_key(key->rsa)) goto cleanup;
	
	/* Get prf key v */
	fread(salt, PRF_KEY_SIZE, 1, pri_key);
	if(ferror(pri_key)) goto cleanup;
	fread(enc_v, 40, 1, pri_key);
	if(ferror(pri_key)) goto cleanup;

	/* Generate a password-based key using PKCS5-PBKDF2 */
	dk = PBKDF2((unsigned char *)password, strlen(password), salt, PRF_KEY_SIZE, 10000, PRP_KEY_SIZE);
	if(!dk) goto cleanup;

	/* Clear out password from memory */
	memset(password, 0, password_len);

	/* NIST-unwrap and strip the padding off of the symetric key, v */
	key_v = nist_key_unwrap(enc_v, 40, dk, PRP_KEY_SIZE);
	if(!key_v) goto cleanup;

	memcpy(key->v, key_v, PRF_KEY_SIZE);
		
	/* Read in the public key */
	rsa = PEM_read_RSAPublicKey(pub_key, NULL, NULL, NULL);
	if(!rsa) goto cleanup;
	RSA_free(rsa);
	
	/* Retreive the generator */
	fread(&gen_size, sizeof(size_t), 1, pub_key);
	if( ((gen = malloc(gen_size)) == NULL)) goto cleanup;
	fread(gen, gen_size, 1, pub_key);
	if(!BN_bin2bn(gen, gen_size, key->g)) goto cleanup;

	if(r1) BN_clear_free(r1);
	if(r2) BN_clear_free(r2);
	if(ctx) BN_CTX_free(ctx);
	if(pkey) EVP_PKEY_free(pkey);
	if(dk) sfree(dk, PRP_KEY_SIZE);
	if(salt) sfree(salt, PRF_KEY_SIZE);
	if(enc_v) sfree(enc_v, 40);
	if(key_v) sfree(key_v, PRF_KEY_SIZE);
	if(gen) sfree(gen, gen_size);
	
	return key;

cleanup:
	memset(password, 0, password_len);
	if(key) destroy_pdp_key(key);
	if(r1) BN_clear_free(r1);
	if(r2) BN_clear_free(r2);
	if(ctx) BN_CTX_free(ctx);
	if(pkey) EVP_PKEY_free(pkey);
	if(dk) sfree(dk, PRP_KEY_SIZE);
	if(salt) sfree(salt, PRF_KEY_SIZE);
	if(enc_v) sfree(enc_v, 40);
	if(key_v) sfree(key_v, PRF_KEY_SIZE);
	if(gen) sfree(gen, gen_size);
	
	return NULL;
}

/* read_pdp_keypair: Read a PDP-keypair from a file and return a PDP_key structure.
 * Takes in two open file pointers to the private and public keys.
 * Returns an allocated PDP_key or NULL on failure.
*/
PDP_key *read_pdp_keypair_temp(FILE *pri_key, FILE *pub_key,char* password_in){

	PDP_key *key = NULL;
	BN_CTX *ctx = NULL;
	BIGNUM *r1 = NULL;
	BIGNUM *r2 = NULL;
	EVP_PKEY *pkey = NULL;
	unsigned char *salt = NULL;
	unsigned char *dk = NULL; /* Derived key */
	unsigned char *enc_v = NULL;
	size_t gen_size = 0;
	unsigned char *gen = NULL;
	size_t password_len = 1024;
	char password[password_len];
	unsigned char *key_v = NULL;
	RSA *rsa = NULL;
	
	if(pri_key == NULL || pub_key == NULL) return NULL;

	if( (key = malloc(sizeof(PDP_key))) == NULL) return NULL;
	memset(key, 0, sizeof(PDP_key));
	if( ((key->g=BN_new()) == NULL)) goto cleanup;
	if( ((key->v = malloc(PRF_KEY_SIZE)) == NULL)) goto cleanup;
	if( ((r1=BN_new()) == NULL)) goto cleanup;
	if( ((r2=BN_new()) == NULL)) goto cleanup;
	if( ((ctx=BN_CTX_new()) == NULL)) goto cleanup;
	if( ((salt = malloc(PRF_KEY_SIZE)) == NULL)) goto cleanup;
	if( ((enc_v = malloc(40)) == NULL)) goto cleanup;
	
	memset(key->v, 0, PRF_KEY_SIZE);
	memset(salt, 0, PRF_KEY_SIZE);
	memset(enc_v, 0, 40);
	memset(password, 0, password_len);

#ifdef	DEBUG_MODE 
	strcpy(password, "z");
#else
	// if(!read_password("Enter passphrase:", (unsigned char *)password, password_len)) goto cleanup;
	strcpy(password,password_in);
	fprintf(stdout,"Password in read pdp keypair:%s\n",password);
#endif

	pkey = PEM_read_PrivateKey(pri_key, NULL, NULL, password);
	if(!pkey){ fprintf(stderr, "ERROR: Failed to decrypt key.\n"); goto cleanup;}
	key->rsa = EVP_PKEY_get1_RSA(pkey);
	if(!key->rsa) goto cleanup;
	
	if(!RSA_check_key(key->rsa)) goto cleanup;
	
	/* Get prf key v */
	fread(salt, PRF_KEY_SIZE, 1, pri_key);
	if(ferror(pri_key)) goto cleanup;
	fread(enc_v, 40, 1, pri_key);
	if(ferror(pri_key)) goto cleanup;

	/* Generate a password-based key using PKCS5-PBKDF2 */
	dk = PBKDF2((unsigned char *)password, strlen(password), salt, PRF_KEY_SIZE, 10000, PRP_KEY_SIZE);
	if(!dk) goto cleanup;

	/* Clear out password from memory */
	memset(password, 0, password_len);

	/* NIST-unwrap and strip the padding off of the symetric key, v */
	key_v = nist_key_unwrap(enc_v, 40, dk, PRP_KEY_SIZE);
	if(!key_v) goto cleanup;

	memcpy(key->v, key_v, PRF_KEY_SIZE);
		
	/* Read in the public key */
	rsa = PEM_read_RSAPublicKey(pub_key, NULL, NULL, NULL);
	if(!rsa) goto cleanup;
	RSA_free(rsa);
	
	/* Retreive the generator */
	fread(&gen_size, sizeof(size_t), 1, pub_key);
	if( ((gen = malloc(gen_size)) == NULL)) goto cleanup;
	fread(gen, gen_size, 1, pub_key);
	if(!BN_bin2bn(gen, gen_size, key->g)) goto cleanup;

	if(r1) BN_clear_free(r1);
	if(r2) BN_clear_free(r2);
	if(ctx) BN_CTX_free(ctx);
	if(pkey) EVP_PKEY_free(pkey);
	if(dk) sfree(dk, PRP_KEY_SIZE);
	if(salt) sfree(salt, PRF_KEY_SIZE);
	if(enc_v) sfree(enc_v, 40);
	if(key_v) sfree(key_v, PRF_KEY_SIZE);
	if(gen) sfree(gen, gen_size);
	
	return key;

cleanup:
	memset(password, 0, password_len);
	if(key) destroy_pdp_key(key);
	if(r1) BN_clear_free(r1);
	if(r2) BN_clear_free(r2);
	if(ctx) BN_CTX_free(ctx);
	if(pkey) EVP_PKEY_free(pkey);
	if(dk) sfree(dk, PRP_KEY_SIZE);
	if(salt) sfree(salt, PRF_KEY_SIZE);
	if(enc_v) sfree(enc_v, 40);
	if(key_v) sfree(key_v, PRF_KEY_SIZE);
	if(gen) sfree(gen, gen_size);
	
	return NULL;
}


/* write_pdp_keypair: writes a PDP_key structure to disk.
*  Takes in a populated PDP_key and the user's passphrase and writes a PEM-PKCS8 encoded private key
*  and NIST-wrapped symmetric key to the private key file and a PEM encoded and raw BIGNUM generator to the
*  public key file
*  returns 1 on success, 0 on failure.
*/
int write_pdp_keypair(PDP_key *key, char *password,char *keypath){

	FILE *pub_key = NULL;
	FILE *pri_key = NULL;
	struct passwd *pw = NULL;
	char pdpkeypath[MAXPATHLEN];
	EVP_PKEY *pkey = NULL;
	unsigned char *salt = NULL;
	unsigned char *dk = NULL;
	unsigned char *enc_v = NULL;
	size_t gen_size = 0;
	unsigned char *gen = NULL;
	unsigned char key_v[32];
	char PathPDPUserDir[MAXPATHLEN];
	char PathPDPPrivateKey[MAXPATHLEN];
	char PathPDPPublicKey[MAXPATHLEN];
	


	if(!key || !password) return 0;
	fprintf(stdout,"test\n");
	if(!keypath){
		strcpy(PathPDPUserDir,".pdp");
		strcpy(PathPDPPrivateKey,".pdp/pdp.pri");
		strcpy(PathPDPPublicKey,".pdp/pdp.pub");
	} else {
		fprintf(stdout,"keypath:%s\n",keypath);
		strcpy(PathPDPPublicKey,keypath);
		strcat(PathPDPPublicKey,"/pdp.pub");
		fprintf(stdout,"PDP Public Key path: %s\n",PathPDPPublicKey);
		strcpy(PathPDPPrivateKey,keypath);
		strcat(PathPDPPrivateKey,"/pdp.pri");
		fprintf(stdout,"PDP Private Key path: %s\n",PathPDPPrivateKey);

		
	}
	
	if( ((pw = getpwuid(getuid())) == NULL) ) goto cleanup;
	
	if( ((pkey = EVP_PKEY_new()) == NULL) ) goto cleanup;
	
	if( ((salt = malloc(PRF_KEY_SIZE)) == NULL)) goto cleanup;
		
	memset(pdpkeypath, 0, MAXPATHLEN);
	memset(salt, 0, PRF_KEY_SIZE);
	memset(key_v, 0, 32);
	
	/* Create ~/.pdp directory if it doesn't already exist. */
	// snprintf(pdpkeypath, MAXPATHLEN, "%s/%s", pw->pw_dir, PathPDPUserDir);
	// if( (access(pdpkeypath, F_OK) != 0)){
	// 	if (mkdir(pdpkeypath, 0700) < 0){
	// 		fprintf(stderr, "Could not create directory '%s'.\n", pdpkeypath);
	// 		goto cleanup;
	// 	}
	// }
	

	

	/* Open, create and truncate the key files */
	// if( snprintf(pdpkeypath, MAXPATHLEN, "%s/%s", pw->pw_dir, PathPDPPrivateKey) < 0 ) goto cleanup;
	pri_key = fopen(PathPDPPrivateKey, "w");
	if(!pri_key) goto cleanup;
	
	// if( snprintf(pdpkeypath, MAXPATHLEN, "%s/%s", pw->pw_dir, PathPDPPublicKey) < 0 ) goto cleanup;
	pub_key = fopen(PathPDPPublicKey, "w");
	if(!pub_key) goto cleanup;
	fprintf(stdout,"PDPKey open success\n");

	/* Turn RSA key into an EVP key and write it in PKCS8 password-protected format */

	if(!EVP_PKEY_set1_RSA(pkey, key->rsa)) goto cleanup;
	
	if(!PEM_write_PKCS8PrivateKey(pri_key, pkey, EVP_aes_256_cbc(), NULL, 0, 0, password)){
		fprintf(stderr, "Did not write private key\n");
		 goto cleanup;
	}




	/* Generate some random bytes for a salt */
	if(!RAND_bytes(salt, PRF_KEY_SIZE)) goto cleanup;

	/* Generate a password-based key using PKCS5-PBKDF2 */
	dk = PBKDF2((unsigned char *)password, strlen(password), salt, PRF_KEY_SIZE, 10000, PRP_KEY_SIZE);
	if(!dk) goto cleanup;

	/* Pad and NIST-wrap the symetric key v */
	memcpy(key_v, key->v, PRF_KEY_SIZE);
	enc_v = nist_key_wrap(key_v, 32, dk, PRP_KEY_SIZE);
	if(!enc_v) goto cleanup;
	
	
	/* Write the salt and encypted value of v */
	fwrite(salt, PRF_KEY_SIZE, 1, pri_key);
	if(ferror(pri_key)) goto cleanup;
	fwrite(enc_v, 40, 1, pri_key);
	if(ferror(pri_key)) goto cleanup;
	
	/* Write the public key */
	if(!PEM_write_RSAPublicKey(pub_key, key->rsa)) goto cleanup;

	/* Write the generator */
	gen_size = BN_num_bytes(key->g);
	fwrite(&gen_size, sizeof(size_t), 1, pub_key);
	if( ((gen = malloc(gen_size)) == NULL)) goto cleanup;
	memset(gen, 0, gen_size);
	if(!BN_bn2bin(key->g, gen)) goto cleanup;
	fwrite(gen, gen_size, 1, pub_key);

	endpwent();
	if(pri_key) fclose(pri_key);
	if(pub_key) fclose(pub_key);
	if(pkey) EVP_PKEY_free(pkey);
	if(dk) sfree(dk, PRP_KEY_SIZE);
	if(salt) sfree(salt, PRF_KEY_SIZE);
	if(enc_v) sfree(enc_v, PRF_KEY_SIZE + 8);
	if(gen) sfree(gen, gen_size);
	
	return 1;
	
cleanup:
	fprintf(stderr, "ERROR: Did not create key pair.\n");
	endpwent();
	if(pri_key) fclose(pri_key);
	if( snprintf(pdpkeypath, MAXPATHLEN, "%s/%s", pw->pw_dir, PathPDPPrivateKey) < 0 ) goto cleanup;
	unlink(pdpkeypath);
	if(pub_key) fclose(pub_key);
	if( snprintf(pdpkeypath, MAXPATHLEN, "%s/%s", pw->pw_dir, PathPDPPublicKey) < 0 ) goto cleanup;
	unlink(pdpkeypath);
	if(pkey) EVP_PKEY_free(pkey);
	if(dk) sfree(dk, PRP_KEY_SIZE);
	if(salt) sfree(salt, PRF_KEY_SIZE);
	if(enc_v) sfree(enc_v, PRF_KEY_SIZE + 8);
	if(gen) sfree(gen, gen_size);
		
	return 0;
}

/* pdp_create_new_keypair: Generates and writes a new PDP key pair to disk.
*  returns an allocated and populated PDP_key structure or NULL on failure.
*/
PDP_key *pdp_create_new_keypair(){

	PDP_key *key = NULL;
	size_t password_len = 1024;
	char password1[password_len];
	char password2[password_len];
	int got_password = 0;
	char prikeypath[MAXPATHLEN];
	char pubkeypath[MAXPATHLEN];
	char yesorno = 0;
	struct passwd *pw = NULL;
		
	memset(password1, 0, password_len);
	memset(password2, 0, password_len);
	memset(prikeypath, 0, MAXPATHLEN);
	memset(pubkeypath, 0, MAXPATHLEN);
	
	if( ((pw = getpwuid(getuid())) == NULL) ) goto cleanup;
	
	snprintf(prikeypath, MAXPATHLEN, "%s/%s", pw->pw_dir, PATH_PDP_PRIVATE_KEY);
	snprintf(pubkeypath, MAXPATHLEN, "%s/%s", pw->pw_dir, PATH_PDP_PUBLIC_KEY);
	if( (access(prikeypath, F_OK) == 0) && (access(pubkeypath, F_OK) == 0)){
		fprintf(stdout, "WARNING: A PDP key pair already exists.  Creating a new key pair\n");
		fprintf(stdout, "will make any previously tagged files unverifiable.\n");
		fprintf(stdout, "Are you sure you want to continue? (y/N) ");
		scanf("%c", &yesorno);
		if(yesorno != 'y') goto cleanup;
	}

	fprintf(stdout, "Generating a new PDP key pair.\n");
	
	/* Get a passphrase from the user */
	do{
		if(!read_password("Enter passphrase:", (unsigned char *)password1, password_len)) goto cleanup;
		if(!read_password("Re-enter passphrase:", (unsigned char *)password2, password_len)) goto cleanup;
	
		if (strcmp(password1, password2) != 0) {
			/* Passwords don't match.  Clear them and try again */
			memset(password1, 0, password_len);
			memset(password2, 0, password_len);
			fprintf(stdout, "Passphrases do not match.  Try again.\n");
		}else
			got_password = 1;
	}while(!got_password);
	

	

	/* Got the password, clear the second one */
	memset(password2, 0, password_len);
	
	

	/* Create a new set of PDP keys */
	key = generate_pdp_key();
	if(!key) goto cleanup;
	
	

	/* Write the new keys to disk */
	if(!write_pdp_keypair(key, password1,NULL)) goto cleanup;
	
	/* Kill the password from memory */
	memset(password1, 0, sizeof(password1));

	endpwent();

	fprintf(stdout, "Your PDP keys have been stored.\n");

	return key;
	
cleanup:
	fprintf(stderr, "ERROR: Unable to create PDP key pair.\n");
	endpwent();
	memset(password1, 0, password_len);
	memset(password2, 0, password_len);
	if(key) destroy_pdp_key(key);
	return NULL;
}

/* pdp_get_keypair: Returns an allocated PDP_key structure containing the private and public keys.
* Keys are read from the private and public key files on disk.
* Returns NULL on failure.
*/
PDP_key *pdp_get_keypair(){

	PDP_key *key = NULL;
	struct passwd *pw = NULL;
	char pdpkeypath[MAXPATHLEN];
	FILE *pri_key = NULL;
	FILE *pub_key = NULL;
	char yesorno = 0;
	char pdppubkey[MAXPATHLEN];
	char pdpprikey[MAXPATHLEN];

	if( ((pw = getpwuid(getuid())) == NULL) ) goto cleanup;
	
	memset(pdpkeypath, 0, MAXPATHLEN);
	
	/* Create the paths to the PDP keys */
	if( snprintf(pdpkeypath, MAXPATHLEN, "%s/%s", pw->pw_dir, PATH_PDP_PRIVATE_KEY) < 0 ) goto cleanup;
	pri_key = fopen(pdpkeypath, "r");
	fprintf(stdout,"pdp private key:%s\n",pri_key);
	if( snprintf(pdpkeypath, MAXPATHLEN, "%s/%s", pw->pw_dir, PATH_PDP_PUBLIC_KEY) < 0 ) goto cleanup;
	pub_key = fopen(pdpkeypath, "r");
	fprintf(stdout,"pdp public key:%s\n",pub_key);	
	fprintf(stdout,"%s\n",pdpkeypath);
	if(pri_key && pub_key){
		key = read_pdp_keypair(pri_key, pub_key);
		if(!key) goto cleanup;
	}else if(!pri_key && !pub_key){
		fprintf(stderr, "ERROR: PDP keys do not exist.\n");
		fprintf(stdout, "Would you like to generate a new pair (y/N)?");
		scanf("%c", &yesorno);
		if(yesorno != 'y') goto cleanup;
		key = pdp_create_new_keypair();
		if(!key) goto cleanup;
	}else if(!pub_key && pri_key){
		/*TODO: Reconstruct public key from private, if it exists */
		fprintf(stderr, "ERROR: PDP public key is missing.\n");
		goto cleanup;

		/* Get user passphrase */
		//if(!readpassphrase("Enter passphrase:", password1, _PASSWORD_LEN + 1, RPP_ECHO_OFF)) goto cleanup;

		/* Reconstrust the key */
		//TODO 
		//key = read_pdp_keypair(pri_key, pub_key, password1);
		//memset(password1, 0, _PASSWORD_LEN + 1);
		//if(!key) goto cleanup;
		
		/* Write the new public key to disk */
		//if( snprintf(pdpkeypath, MAXPATHLEN, "%s/%s", pw->pw_dir, PATH_PDP_PUBLIC_KEY) < 0 ) goto cleanup;
		//pub_key = fopen(pdpkeypath, "w");
		//if(!PEM_write_RSAPublicKey(pub_key, key->rsa)) goto cleanup;

	}else{
		fprintf(stderr, "ERROR: PDP private key is missing.\n");
		goto cleanup;
	}
	
	if(pri_key) fclose(pri_key);
	if(pub_key) fclose(pub_key);
	endpwent();
	
	return key;
	
cleanup:
	fprintf(stderr, "ERROR: Unable to access your PDP keys.\n");
	endpwent();
	if(key) destroy_pdp_key(key);
	if(pri_key) fclose(pri_key);
	if(pub_key) fclose(pub_key);
	
	return NULL;
	
}

/* pdp_get_keypair: Returns an allocated PDP_key structure containing the private and public keys.
* Keys are read from the private and public key files on disk.
* Returns NULL on failure.
*/
PDP_key *pdp_get_keypair_temp(char* keypath,char* password){

	PDP_key *key = NULL;
	struct passwd *pw = NULL;
	char pdpkeypath[MAXPATHLEN];
	FILE *pri_key = NULL;
	FILE *pub_key = NULL;
	char yesorno = 0;
	char pdppubkey[MAXPATHLEN];
	char pdpprikey[MAXPATHLEN];

	strcpy(pdppubkey,keypath);
	strcat(pdppubkey,"/pdp.pub");
	strcpy(pdpprikey,keypath);
	strcat(pdpprikey,"/pdp.pri");

	if( ((pw = getpwuid(getuid())) == NULL) ) goto cleanup;
	
	memset(pdpkeypath, 0, MAXPATHLEN);
	
	/* Create the paths to the PDP keys */
	// if( snprintf(pdpkeypath, MAXPATHLEN, "%s/%s", pw->pw_dir, PATH_PDP_PRIVATE_KEY) < 0 ) goto cleanup;
	pri_key = fopen(pdpprikey, "r");
	fprintf(stdout,"pdp private key:%s\n",pdpprikey);
	// if( snprintf(pdpkeypath, MAXPATHLEN, "%s/%s", pw->pw_dir, PATH_PDP_PUBLIC_KEY) < 0 ) goto cleanup;
	pub_key = fopen(pdppubkey, "r");
	fprintf(stdout,"pdp public key:%s\n",pdppubkey);	
	fprintf(stdout,"%s\n",pdpkeypath);
	if(pri_key && pub_key){
		key = read_pdp_keypair_temp(pri_key, pub_key,password);
		if(!key) goto cleanup;
	}else if(!pri_key && !pub_key){
		fprintf(stderr, "ERROR: PDP keys do not exist.\n");
		// fprintf(stdout, "Would you like to generate a new pair (y/N)?");
		// scanf("%c", &yesorno);
		// if(yesorno != 'y') goto cleanup;
		// key = pdp_create_new_keypair();
		// if(!key) goto cleanup;
	}else if(!pub_key && pri_key){
		/*TODO: Reconstruct public key from private, if it exists */
		fprintf(stderr, "ERROR: PDP public key is missing.\n");
		goto cleanup;

		/* Get user passphrase */
		//if(!readpassphrase("Enter passphrase:", password1, _PASSWORD_LEN + 1, RPP_ECHO_OFF)) goto cleanup;

		/* Reconstrust the key */
		//TODO 
		//key = read_pdp_keypair(pri_key, pub_key, password1);
		//memset(password1, 0, _PASSWORD_LEN + 1);
		//if(!key) goto cleanup;
		
		/* Write the new public key to disk */
		//if( snprintf(pdpkeypath, MAXPATHLEN, "%s/%s", pw->pw_dir, PATH_PDP_PUBLIC_KEY) < 0 ) goto cleanup;
		//pub_key = fopen(pdpkeypath, "w");
		//if(!PEM_write_RSAPublicKey(pub_key, key->rsa)) goto cleanup;

	}else{
		fprintf(stderr, "ERROR: PDP private key is missing.\n");
		goto cleanup;
	}
	
	if(pri_key) fclose(pri_key);
	if(pub_key) fclose(pub_key);
	endpwent();
	
	return key;
	
cleanup:
	fprintf(stderr, "ERROR: Unable to access your PDP keys.\n");
	endpwent();
	if(key) destroy_pdp_key(key);
	if(pri_key) fclose(pri_key);
	if(pub_key) fclose(pub_key);
	
	return NULL;
	
}


/* pdp_get_pubkey: Returns an PDP_key structure with only the public-key components allocated or NULL on failure.
*/
PDP_key *pdp_get_pubkey(){

	PDP_key *key = NULL;
	size_t gen_size = 0;
	unsigned char *gen = NULL;
	FILE *pub_key = NULL;
	char pdpkeypath[MAXPATHLEN];
	struct passwd *pw = NULL;
	
	if( ((pw = getpwuid(getuid())) == NULL) ) return NULL;
	memset(pdpkeypath, 0, MAXPATHLEN);
	
	if( snprintf(pdpkeypath, MAXPATHLEN, "%s/%s", pw->pw_dir, PATH_PDP_PUBLIC_KEY) < 0 ) goto cleanup;
	pub_key = fopen(pdpkeypath, "r");
	if(!pub_key) return NULL;
	
	if( (key = malloc(sizeof(PDP_key))) == NULL) return NULL;
	memset(key, 0, sizeof(PDP_key));
	if( ((key->g=BN_new()) == NULL)) goto cleanup;
	
	/* Read in the public key */
	key->rsa = PEM_read_RSAPublicKey(pub_key, NULL, NULL, NULL);
	if(!key->rsa) goto cleanup;
	
	/* Retreive the generator */
	fread(&gen_size, sizeof(size_t), 1, pub_key);
	if( ((gen = malloc(gen_size)) == NULL)) goto cleanup;
	fread(gen, gen_size, 1, pub_key);
	if(!BN_bin2bn(gen, gen_size, key->g)) goto cleanup;
	
	if(gen) sfree(gen, gen_size);
	endpwent();
		
	return key;
	
cleanup:
	fprintf(stderr, "ERROR: Unable to access your PDP public key.\n");
	if(key) destroy_pdp_key(key);
	if(gen) sfree(gen, gen_size);
	endpwent();
	
	return NULL;
}

/* destroy_pdp_key: Zero and free the memory in a PDP_key structure */
void destroy_pdp_key(PDP_key *key){
	
	if(!key) return;
	if(key->rsa) RSA_free(key->rsa);
	if(key->v)  sfree(key->v, PRF_KEY_SIZE);
	if(key->g) destroy_pdp_generator(key->g);
	if(key) sfree(key, sizeof(PDP_key));
	key = NULL;
}

/* generate_pdp_key: Generate a new PDP key pair and popular a PDP_key structure.
*  Returns an allocated PDP_key strucutre or NULL on failure.
*/
PDP_key *generate_pdp_key(){

	PDP_key *key = NULL;
	BN_CTX *ctx = NULL;
	BIGNUM *r1 = NULL;
	BIGNUM *r2 = NULL;
	BIGNUM *phi = NULL;
	BIGNUM *p = NULL;
	BIGNUM *q = NULL;
	BIGNUM *d = NULL;
	BIGNUM *e = NULL;
	BIGNUM *n = NULL;
	BIGNUM *dmp1 = NULL;
	BIGNUM *dmq1 = NULL;
	BIGNUM *iqmp = NULL;

	if( (key = malloc(sizeof(PDP_key))) == NULL) return NULL;
	memset(key, 0, sizeof(PDP_key));
	
	if( ((p=BN_new()) == NULL)) goto cleanup;
	if( ((q=BN_new()) == NULL)) goto cleanup;
	if( ((d=BN_new()) == NULL)) goto cleanup;
	if( ((e=BN_new()) == NULL)) goto cleanup;
	if( ((n=BN_new()) == NULL)) goto cleanup;
	if( ((dmp1=BN_new()) == NULL)) goto cleanup;
	if( ((dmq1=BN_new()) == NULL)) goto cleanup;
	if( ((iqmp=BN_new()) == NULL)) goto cleanup;

	if( ((r1=BN_new()) == NULL)) goto cleanup;
	if( ((r2=BN_new()) == NULL)) goto cleanup;
	if( ((ctx=BN_CTX_new()) == NULL)) goto cleanup;
	if( ((phi=BN_new()) == NULL)) goto cleanup;
	if( ((key->v = malloc(PRF_KEY_SIZE)) == NULL)) goto cleanup;
	memset(key->v, 0, PRF_KEY_SIZE);
	
#ifdef USE_SAFE_PRIMES


	/* Generate two safe primes p and q */
  	if(!BN_generate_prime_ex(p, (RSA_KEY_SIZE/2), 1, NULL, NULL, NULL)) goto cleanup;
	if(!BN_is_prime_ex(p, BN_prime_checks, ctx, NULL)) goto cleanup;
	
  	if(!BN_generate_prime(q, (RSA_KEY_SIZE/2), 1, NULL, NULL, NULL, NULL)) goto cleanup;
	if(!BN_is_prime(q, BN_prime_checks, NULL, ctx, NULL)) goto cleanup;
	if(BN_cmp(p,q) == 0) goto cleanup;
	
	/* Create an RSA modulus N*/
	if(!BN_mul(n, p, q, ctx)) goto cleanup;
		
	/* Set e */
	if(!BN_set_word(e, RSA_E)) goto cleanup;	

	/* Generate phi and d */
	if (!BN_sub(r1, p, BN_value_one())) goto cleanup;	/* p-1 */
	if (!BN_sub(r2, q, BN_value_one())) goto cleanup;	/* q-1 */
	if (!BN_mul(phi, r1, r2, ctx)) goto cleanup;	/* phi = (p-1)(q-1) */
	if (!BN_mod_inverse(d, e, phi, ctx)) goto cleanup;	/* d */	
	
	/* Calculate d mod (p-1) */
	if (!BN_mod(dmp1, d, r1, ctx)) goto cleanup;
	
	/* Calculate d mod (q-1) */
	if (!BN_mod(dmq1, d, r2, ctx)) goto cleanup;
	
	/* Calculate the inverse of q mod p */
	if (!BN_mod_inverse(iqmp, q, p, ctx)) goto cleanup;
	if( ((key->rsa = RSA_new()) == NULL)) goto cleanup;
	if(	(RSA_set0_key(key->rsa,n,e,d)==NULL)) goto cleanup;
	if(	(RSA_set0_factors(key->rsa,p,q)==NULL)) goto cleanup;
	if(	(RSA_set0_crt_params(key->rsa,dmp1,dmq1,iqmp)==NULL)) goto cleanup;
	
#else
	if( ((key->rsa = RSA_generate_key(RSA_KEY_SIZE, RSA_E, NULL, NULL)) == NULL)) goto cleanup;
#endif

	/* Check the RSA key pair */
	if(!RSA_check_key(key->rsa)) goto cleanup;
	/* Generate symmetric keys */
	if(!RAND_bytes(key->v, PRF_KEY_SIZE)) goto cleanup;

	
	/* Pick a PDP generator */
	if( ((key->g = pick_pdp_generator(RSA_get0_n(key->rsa))) == NULL)) goto cleanup;
	
	/* We're done, free memory and return keys */
	if(ctx) BN_CTX_free(ctx);
	if(r1) BN_clear_free(r1);
	if(r2) BN_clear_free(r2);
	if(phi) BN_clear_free(phi);
	// if(p) BN_clear_free(p);
	// if(q) BN_clear_free(q);
	// if(d) BN_clear_free(d);
	// if(e) BN_clear_free(e);
	// if(n) BN_clear_free(n);
	// if(dmp1) BN_clear_free(dmp1);
	// if(dmq1) BN_clear_free(dmq1);
	// if(iqmp) BN_clear_free(iqmp);

	/* Check the RSA key pair */
	if(!RSA_check_key(key->rsa)) goto cleanup;

	return key;
	
cleanup:
	if(key)	destroy_pdp_key(key);
	if(r1) BN_clear_free(r1);
	if(r2) BN_clear_free(r2);
	if(phi) BN_clear_free(phi);
	if(p) BN_clear_free(p);
	if(q) BN_clear_free(q);
	if(d) BN_clear_free(d);
	if(e) BN_clear_free(e);
	if(n) BN_clear_free(n);
	if(dmp1) BN_clear_free(dmp1);
	if(dmq1) BN_clear_free(dmq1);
	if(iqmp) BN_clear_free(iqmp);
	if(ctx) BN_CTX_free(ctx);


	return NULL;
}
