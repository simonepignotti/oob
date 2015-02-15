# oob
This simple (but efficient) protocol is based on this idea:
- client c encrypts message m with an integer i;
- c sends its ID to a random server;
- after i milliseconds, c repeats the previous point. This will happen a certain number of times (fixed);
- When the communication finishes, all the servers send their estimations to the supervisor;
- The supervisor takes the best estimation (potentially the gcd) of all the servers.

Sockets are AF_UNIX, so for the moment the exchange of messages can take place just between processes running on the same machine. All the code respects the POSIX standard. Sorry for the italian documentation, please contact me for any doubt or question.
