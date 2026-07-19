#ifndef __CGMINER_APIBRIDGE_H__
#define __CGMINER_APIBRIDGE_H__

#ifdef USE_APIBRIDGE

/* Fork/exec the apibridge companion process once the local API socket is
 * confirmed listening, and start a babysitter thread that respawns it on
 * unexpected exit. No-op if --api-bridge was not passed. */
void start_apibridge(void);

/* Stop the babysitter and terminate the apibridge child cleanly. Safe to
 * call even if start_apibridge() never started anything. */
void stop_apibridge(void);

#endif /* USE_APIBRIDGE */

#endif /* __CGMINER_APIBRIDGE_H__ */
