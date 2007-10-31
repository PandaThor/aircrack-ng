  /*
   *  Copyright (c) 2007, Andrea Bittau <a.bittau@cs.ucl.ac.uk>
   *
   *  OS dependent API for cygwin. It relies on an external
   *  DLL to do the actual wifi stuff
   *
   *  This program is free software; you can redistribute it and/or modify
   *  it under the terms of the GNU General Public License as published by
   *  the Free Software Foundation; either version 2 of the License, or
   *  (at your option) any later version.
   *
   *  This program is distributed in the hope that it will be useful,
   *  but WITHOUT ANY WARRANTY; without even the implied warranty of
   *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   *  GNU General Public License for more details.
   *
   *  You should have received a copy of the GNU General Public License
   *  along with this program; if not, write to the Free Software
   *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
   */

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "osdep.h"
#include "network.h"

#ifdef HAVE_AIRPCAP
	#include "airpcap.h"
#endif

// DLL function that have to be exported
#define CYGWIN_DLL_INIT		"cygwin_init"
#define CYGWIN_DLL_SET_CHAN	"cygwin_set_chan"
#define CYGWIN_DLL_INJECT	"cygwin_inject"
#define CYGWIN_DLL_SNIFF	"cygwin_sniff"
#define CYGWIN_DLL_GET_MAC	"cygwin_get_mac"
#define CYGWIN_DLL_SET_MAC	"cygwin_set_mac"
#define CYGWIN_DLL_CLOSE	"cygwin_close"

#define DLL_EXTENSION ".dll"

struct priv_cygwin {
	pthread_t	pc_reader;
	volatile int	pc_running;
	int		pc_pipe[2]; /* reader -> parent */
	int		pc_channel;
	struct wif	*pc_wi;
	int		pc_did_init;

	int		isAirpcap;
	int		useDll;

	int		(*pc_init)(char *param);
	int		(*pc_set_chan)(int chan);
	int		(*pc_inject)(void *buf, int len, struct tx_info *ti);
	int		(*pc_sniff)(void *buf, int len, struct rx_info *ri);
	int		(*pc_get_mac)(void *mac);
	int		(*pc_set_mac)(void *mac);
	void		(*pc_close)(void);
};

char *stristr(const char *String, const char *Pattern)
{
      char *pptr, *sptr, *start;
      uint  slen, plen;

      for (start = (char *)String,
           pptr  = (char *)Pattern,
           slen  = strlen(String),
           plen  = strlen(Pattern);

           /* while string length not shorter than pattern length */

           slen >= plen;

           start++, slen--)
      {
            /* find start of pattern in string */
            while (toupper(*start) != toupper(*Pattern))
            {
                  start++;
                  slen--;

                  /* if pattern longer than string */

                  if (slen < plen)
                        return(NULL);
            }

            sptr = start;
            pptr = (char *)Pattern;

            while (toupper(*sptr) == toupper(*pptr))
            {
                  sptr++;
                  pptr++;

                  /* if end of pattern then pattern was found */

                  if ('\0' == *pptr)
                        return (start);
            }
      }
      return(NULL);
}

static int do_cygwin_open(struct wif *wi, char *iface)
{
	struct priv_cygwin *priv = wi_priv(wi);
	void *lib;
	char *file;
	char *parm;
	int rc = -1;
	int tempret = 0;

	if (!iface)
		return -1;
	if (strlen(iface) == 0)
		return -1;

	priv->useDll = 0;

	if (stristr(iface, DLL_EXTENSION))
		priv->useDll = 1;

	if (priv->useDll)
	{
		file = strdup(iface);
		if (!file)
			return -1;

		parm = strchr(file, '|');
		if (parm)
			*parm++ = 0;

		/* load lib */
		lib = dlopen(file, RTLD_LAZY);
		if (!lib)
			goto errdll;

		priv->pc_init		= dlsym(lib, CYGWIN_DLL_INIT);
		priv->pc_set_chan	= dlsym(lib, CYGWIN_DLL_SET_CHAN);
		priv->pc_get_mac	= dlsym(lib, CYGWIN_DLL_GET_MAC);
		priv->pc_set_mac	= dlsym(lib, CYGWIN_DLL_SET_MAC);
		priv->pc_close		= dlsym(lib, CYGWIN_DLL_CLOSE);
		priv->pc_inject		= dlsym(lib, CYGWIN_DLL_INJECT);
		priv->pc_sniff		= dlsym(lib, CYGWIN_DLL_SNIFF);

		if (!(priv->pc_init && priv->pc_set_chan && priv->pc_get_mac
			  && priv->pc_inject && priv->pc_sniff && priv->pc_close))
			goto errdll;

		/* init lib */
		if ((rc = priv->pc_init(parm)))
			goto errdll;
		priv->pc_did_init = 1;

		rc = 0;

errdll:
		free(file);
	}
	else
	{
		#ifdef HAVE_AIRPCAP
			priv->isAirpcap = isAirpcapDevice(iface);
			if (priv->isAirpcap)
			{
				priv->pc_init		= airpcap_init;
				priv->pc_set_chan	= airpcap_set_chan;
				priv->pc_get_mac	= airpcap_get_mac;
				priv->pc_set_mac	= airpcap_set_mac;
				priv->pc_close		= airpcap_close;
				priv->pc_inject		= airpcap_inject;
				priv->pc_sniff		= airpcap_sniff;

				rc = 0;
			}

		#endif

	}

	// Show an error message if the adapter is not supported
	if (rc == 0)
	{
		/* set initial chan */
		tempret = wi_set_channel(wi, 1);
		if (tempret)
			rc = tempret;
	}
	else
	{
		fprintf(stderr, "Adapter <%s> not supported\n", iface);
	}

	return rc;
}

static int cygwin_set_channel(struct wif *wi, int chan)
{
	struct priv_cygwin *priv = wi_priv(wi);

	if (priv->pc_set_chan(chan) == -1)
		return -1;

	priv->pc_channel = chan;
	return 0;
}

static int cygwin_read_packet(struct priv_cygwin *priv, void *buf, int len,
			      struct rx_info *ri)
{
	int rd;

	memset(ri, 0, sizeof(*ri));

	rd = priv->pc_sniff(buf, len, ri);
	if (rd == -1)
		return -1;

	if (!ri->ri_channel)
		ri->ri_channel = wi_get_channel(priv->pc_wi);

	return rd;
}

static int cygwin_write(struct wif *wi, unsigned char *h80211, int len,
			struct tx_info *ti)
{
	struct priv_cygwin *priv = wi_priv(wi);
	int rc;

	if ((rc = priv->pc_inject(h80211, len, ti)) == -1)
		return -1;

	return rc;
}

static int cygwin_get_channel(struct wif *wi)
{
	struct priv_cygwin *pc = wi_priv(wi);

	return pc->pc_channel;
}

int cygwin_read_reader(int fd, int plen, void *dst, int len)
{
	/* packet */
	if (len > plen)
		len = plen;
	if (net_read_exact(fd, dst, len) == -1)
		return -1;
	plen -= len;

	/* consume packet */
	while (plen) {
		char lame[1024];
		int rd = sizeof(lame);

		if (rd > plen)
			rd = plen;

		if (net_read_exact(fd, lame, rd) == -1)
			return -1;

		plen -= rd;

		assert(plen >= 0);
	}

	return len;
}

static int cygwin_read(struct wif *wi, unsigned char *h80211, int len,
		       struct rx_info *ri)
{
	struct priv_cygwin *pc = wi_priv(wi);
	struct rx_info tmp;
	int plen;

	if (pc->pc_running == -1)
		return -1;

	if (!ri)
		ri = &tmp;

	/* length */
	if (net_read_exact(pc->pc_pipe[0], &plen, sizeof(plen)) == -1)
		return -1;

	/* ri */
	if (net_read_exact(pc->pc_pipe[0], ri, sizeof(*ri)) == -1)
		return -1;
	plen -= sizeof(*ri);
	assert(plen > 0);

	return cygwin_read_reader(pc->pc_pipe[0], plen, h80211, len);
}

static void do_free(struct wif *wi)
{
	struct priv_cygwin *pc = wi_priv(wi);
	int tries = 3;

	/* wait for reader */
	if (pc->pc_running == 1) {
		pc->pc_running = 0;

		while ((pc->pc_running != -1) && tries--)
			sleep(1);
	}

	if (pc->pc_pipe[0]) {
		close(pc->pc_pipe[0]);
		close(pc->pc_pipe[1]);
	}

	if (pc->pc_did_init)
		pc->pc_close();

	assert(wi->wi_priv);
	free(wi->wi_priv);
	wi->wi_priv = 0;

	free(wi);
}

static void cygwin_close(struct wif *wi)
{
	do_free(wi);
}

static int cygwin_fd(struct wif *wi)
{
	struct priv_cygwin *pc = wi_priv(wi);

	if (pc->pc_running == -1)
		return -1;

	return pc->pc_pipe[0];
}

static int cygwin_get_mac(struct wif *wi, unsigned char *mac)
{
	struct priv_cygwin *pc = wi_priv(wi);

	return pc->pc_get_mac(mac);
}

static int cygwin_set_mac(struct wif *wi, unsigned char *mac)
{
	struct priv_cygwin *pc = wi_priv(wi);

	return pc->pc_set_mac(mac);
}

static int cygwin_get_monitor(struct wif *wi)
{
	if (wi) {} /* XXX unused */

	return 0;
}

static int cygwin_get_rate(struct wif *wi)
{
	if (wi) {} /* XXX unused */

	return 1000000;
}

static int cygwin_set_rate(struct wif *wi, int rate)
{
	if (wi || rate) {} /* XXX unused */

	return 0;
}

static void *cygwin_reader(void *arg)
{
	struct priv_cygwin *priv = arg;
	unsigned char buf[2048];
	int len;
	struct rx_info ri;

	while (priv->pc_running) {
		/* read one packet */
		len = cygwin_read_packet(priv, buf, sizeof(buf), &ri);
		if (len == -1)
			break;

		/* len */
		len += sizeof(ri);
		if (write(priv->pc_pipe[1], &len, sizeof(len)) != sizeof(len))
			break;
		len -= sizeof(ri);

		/* ri */
		if (write(priv->pc_pipe[1], &ri, sizeof(ri)) != sizeof(ri))
			break;

		/* packet */
		if (write(priv->pc_pipe[1], buf, len) != len)
			break;
	}

	priv->pc_running = -1;
	return NULL;
}

static struct wif *cygwin_open(char *iface)
{
	struct wif *wi;
	struct priv_cygwin *priv;

	/* setup wi struct */
	wi = wi_alloc(sizeof(*priv));
	if (!wi)
		return NULL;
	wi->wi_read		= cygwin_read;
	wi->wi_write		= cygwin_write;
	wi->wi_set_channel	= cygwin_set_channel;
	wi->wi_get_channel	= cygwin_get_channel;
	wi->wi_close		= cygwin_close;
	wi->wi_fd		= cygwin_fd;
	wi->wi_get_mac		= cygwin_get_mac;
	wi->wi_set_mac		= cygwin_set_mac;
	wi->wi_get_rate		= cygwin_get_rate;
	wi->wi_set_rate		= cygwin_set_rate;
        wi->wi_get_monitor      = cygwin_get_monitor;

	/* setup iface */
	if (do_cygwin_open(wi, iface) == -1)
		goto err;

	/* setup private state */
	priv = wi_priv(wi);
	priv->pc_wi = wi;

	/* setup reader */
	if (pipe(priv->pc_pipe) == -1)
		goto err;
	priv->pc_running = 2;
	if (pthread_create(&priv->pc_reader, NULL, cygwin_reader, priv))
		goto err;
	priv->pc_running = 1;

	return wi;

err:
	do_free(wi);
	return NULL;
}

struct wif *wi_open_osdep(char *iface)
{
	return cygwin_open(iface);
}

int get_battery_state(void)
{
	/* XXX use winapi */
	return -1;
}
