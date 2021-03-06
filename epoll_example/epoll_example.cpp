
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>

#include <stdio.h>

#include <iostream>

#include "epoll_example.h"

bool epoller::removeOrClose (int iFd, bool bClose)
{
  bool bFound = false;
  std::set<int>::iterator iter;

  // remove from the list of monitored descriptors
  iter = mset_fileDescriptors.find (iFd);
  if (iter != mset_fileDescriptors.end())
  {
    // remove it from monitoring, explicitly
    epoll_ctl (mi_PollFd, EPOLL_CTL_DEL, iFd, &mv_event[0]);

    if (bClose == true)
    {
      if (iFd != STDIN_FILENO)
      {
        // close - but not if it's stdin
        if (::close (iFd) == -1)
        {
          perror ("close");
          exit (1);
        }
        else
        {
          makeFileDescriptorBlocking (iFd);
        }
      }
      else
      {
        makeFileDescriptorBlocking (iFd);
      }
    }
    bFound = true;
    
    // remove this from the list of monitored file descriptors
    mset_fileDescriptors.erase (iter);
    
    // shrink the list of events by one
    mv_event.erase (mv_event.end());
  }
  return bFound;
}

epoller::epoller ()
{
  mi_PollFd = epoll_create1 (0);
  if (mi_PollFd == -1)
  {
    perror ("epoll_create1");
    exit (1);
  }
  mi_Ready = 0;
}

void epoller::makeFileDescriptorNonBlocking (int iFd)
{
  int iFlags;
  int iNewFd;

  iFlags = fcntl (iFd, F_GETFL, 0);
  if (iFlags == -1)
  {
    perror ("fcntl");
    exit (1);
  }

  iFlags |= O_NONBLOCK;
  iNewFd = fcntl (iFd, F_SETFL, iFlags);
  if (iNewFd == -1)
  {
    perror ("fcntl");
    exit (1);
  }
}

void epoller::makeFileDescriptorBlocking (int iFd)
{
  int iFlags;
  int iNewFd;

  iFlags = fcntl (iFd, F_GETFL, 0);
  if (iFlags == -1)
  {
    perror ("fcntl");
    exit (1);
  }

  iFlags &= ~O_NONBLOCK;
  iNewFd = fcntl (iFd, F_SETFL, iFlags);
  if (iNewFd == -1)
  {
    perror ("fcntl");
    exit (1);
  }
}

bool epoller::add (int iFd, const epoll_data &epData)
{
  struct epoll_event event;
  int iRet;
  bool bNew = true;

  if (mset_fileDescriptors.find(iFd) != mset_fileDescriptors.end())
  {
    bNew = false;
    std::cout << "Duplicate\n";
  }
  else
  {
    makeFileDescriptorNonBlocking (iFd);

    event.data = epData;
    event.events = EPOLLIN | EPOLLET;
    iRet = epoll_ctl (mi_PollFd, EPOLL_CTL_ADD, iFd, &event);
    
    if (iRet == -1)
    {
      perror ("epoll_ctl");
      exit (1);
    }
    
    // make room for returned events, data within event is irrelevant
    mv_event.push_back (event);

    // keep a record of what file descriptors we're monitoring
    mset_fileDescriptors.insert (iFd);
  }

  return bNew;
}

epoll_event epoller::wait (int iTimeout)
{
  int iRet;
  epoll_event epEvent;

  epEvent.events = 0;
  epEvent.data.ptr = NULL;

  if (mi_Ready == 0)
  {
    // if no file descriptor to process
    iRet = epoll_wait (mi_PollFd, &(mv_event[0]), mv_event.size(), iTimeout);

    if (iRet == -1 && errno == EINTR)
    {
      std::cout << "Signal trapped\n";
    }
    else if (iRet > 0)
    {
      mi_Ready = iRet;
    }
    else
    {
      // timeout
      epEvent.events = 0;
    }
  }

  if (mi_Ready > 0)
  {
    // with already have file descriptors to process
    mi_Ready--;
    epEvent = mv_event[mi_Ready];
  }

  return epEvent;
}

epoller::~epoller ()
{
  for (std::set<int>::iterator iter = mset_fileDescriptors.begin() ; iter != mset_fileDescriptors.end() ; iter++)
  {
    if (*iter != STDIN_FILENO) // don't want to close stdin, if we've used it
    {
      if (::close (*iter) == -1)
      {
        perror ("close");
        exit (1);
      }
      else
      {
        makeFileDescriptorBlocking (*iter);
      }
    }
  }
    
  // close the epoll fd
  if (::close (mi_PollFd) == -1)
  {
    perror ("close");
    exit (1);
  }
}

#if 0
// stole this code from https://stackoverflow.com/questions/1798511/how-to-avoid-pressing-enter-with-getchar#1798833
#include <termios.h>
int blah (void)
{
  int c;   
  static struct termios oldt, newt;

  /*tcgetattr gets the parameters of the current terminal
    STDIN_FILENO will tell tcgetattr that it should write the settings
    of stdin to oldt*/
  tcgetattr( STDIN_FILENO, &oldt);
  /*now the settings will be copied*/
  newt = oldt;

  /*ICANON normally takes care that one line at a time will be processed
    that means it will return if it sees a "\n" or an EOF or an EOL*/
  // also turned off echo
  newt.c_lflag &= ~(ICANON) & ~(ECHO);          

  /*Those new settings will be set to STDIN
    TCSANOW tells tcsetattr to change attributes immediately. */
  tcsetattr( STDIN_FILENO, TCSANOW, &newt);

  /*This is your part:
    I choose 'e' to end input. Notice that EOF is also turned off
    in the non-canonical mode*/
  while((c=getchar())!= 'e')
  {
    //putchar(c);
    printf ("%c", c+1);
    fflush (stdout);
  }

  /*restore the old settings*/
  tcsetattr( STDIN_FILENO, TCSANOW, &oldt);

  return 0;
}
#endif

int main (int argc, char **argv)
{
  epoller ep;
  const int iTimeout = 5000;

  printf ("Type 3 lines.  You have %d ms to type each line before timeout\n", iTimeout);
  
  ep.add (STDIN_FILENO);
  for (int i = 0 ; i < 3 ; i++)
  {
    struct epoll_event event;
    int iRet;
    unsigned char var;
    
    event = ep.wait (iTimeout);
    printf (".events = %08x : ", event.events);
    if (event.events != 0)
    {
      for ( ;; )
      {
        iRet = read (event.data.fd, &var, sizeof (var));
        if (iRet == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
          break;
        }
        std::cout << iRet << ":" << var << " ";
        fflush (stdout);
      }
    }
    else
    {
      printf ("timeout\n");
    }
  }

  return 0;
}
