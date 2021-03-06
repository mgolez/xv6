// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "traps.h"
#include "spinlock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

static void consputc(int);

static int panicked = 0;

static struct {
  struct spinlock lock;
  int locking;
} cons;

static void
printint(int xx, int base, int sign)
{
  static char digits[] = "0123456789abcdef";
  char buf[16];
  int i;
  uint x;

  if(sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  i = 0;
  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}
//PAGEBREAK: 50

// Print to the console. only understands %d, %x, %p, %s.
void
cprintf(char *fmt, ...)
{
  int i, c, locking;
  uint *argp;
  char *s;

  locking = cons.locking;
  if(locking)
    acquire(&cons.lock);

  if (fmt == 0)
    panic("null fmt");

  argp = (uint*)(void*)(&fmt + 1);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(*argp++, 10, 1);
      break;
    case 'x':
    case 'p':
      printint(*argp++, 16, 0);
      break;
    case 's':
      if((s = (char*)*argp++) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }

  if(locking)
    release(&cons.lock);
}

void
panic(char *s)
{
  int i;
  uint pcs[10];
  
  cli();
  cons.locking = 0;
  cprintf("cpu%d: panic: ", cpu->id);
  cprintf(s);
  cprintf("\n");
  getcallerpcs(&s, pcs);
  for(i=0; i<10; i++)
    cprintf(" %p", pcs[i]);
  panicked = 1; // freeze other CPU
  for(;;)
    ;
}

//PAGEBREAK: 50
#define BACKSPACE 0x100
#define CRTPORT 0x3d4
#define KEY_LF 0xE4
#define KEY_RT 0xE5
#define KEY_UP 0xE2
#define KEY_DN 0xE3
static ushort *crt = (ushort*)P2V(0xb8000);  // CGA memory

static void
cgaputc(int c)
{
  int pos;
  
  // Cursor position: col + 80*row.
  outb(CRTPORT, 14);
  pos = inb(CRTPORT+1) << 8;
  outb(CRTPORT, 15);
  pos |= inb(CRTPORT+1);

  if(c == '\n')
    pos += 80 - pos%80;
  else if(c==KEY_RT)
  {
    ++pos;
  }
  else if(c == BACKSPACE || c==KEY_LF){
    if(pos > 0) --pos;
  } else
    crt[pos++] = (c&0xff) | 0x0700;  // black on white
  
  if((pos/80) >= 24){  // Scroll up.
    memmove(crt, crt+80, sizeof(crt[0])*23*80);
    pos -= 80;
    memset(crt+pos, 0, sizeof(crt[0])*(24*80 - pos));
  }
  
  outb(CRTPORT, 14);
  outb(CRTPORT+1, pos>>8);
  outb(CRTPORT, 15);
  outb(CRTPORT+1, pos);
  if(c == BACKSPACE)
    crt[pos] = ' ' | 0x0700;
}

void
consputc(int c)
{
  if(panicked){
    cli();
    for(;;)
      ;
  }

  if(c == BACKSPACE){
    uartputc('\b'); uartputc(' '); uartputc('\b');
  } else
    uartputc(c);
  cgaputc(c);
}

#define INPUT_BUF 128
struct {
  struct spinlock lock;
  char buf[INPUT_BUF];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index - if press arrow-left this index save the rightest index
  uint f;  // Current edit index - Can move by arrows 
} input;
#define MAX_HISTORY_LENGTH 20
char History[MAX_HISTORY_LENGTH][INPUT_BUF];
int HistoryPos = 0;
int MaxHistoryPos = 0;
int HistoryWrite = 0;

#define C(x)  ((x)-'@')  // Control-x

void SelectFromHistory()
{
  // Change the view
      // Go to the right
      while((input.e - input.f) > 0)
      {
        input.f++;
        cgaputc(KEY_RT);
      }
      // Delete Current line
      while(input.e > input.w)
      {
        cgaputc(BACKSPACE);
        ++input.w; // Here also change the logic
      }
      input.e = input.w;
      input.f = input.w;
      input.r = input.w;

      // Insert command from History
      char Newc;
      int j;
      for(j = 0;  History[HistoryPos][j] != '\0'; ++j)
      {
        Newc = History[HistoryPos][j];

        input.buf[input.f++ % INPUT_BUF] = Newc;
        ++input.e;
        consputc(Newc);
      } 
}

void
consoleintr(int (*getc)(void))
{
  int c;

  acquire(&input.lock);
  while((c = getc()) >= 0){
    switch(c){
    case C('P'):  // Process listing.
      procdump();
      break;
    case C('U'):  // Kill line.
      while(input.e != input.w &&
            input.buf[(input.e-1) % INPUT_BUF] != '\n'){
        input.e--;
        consputc(BACKSPACE);
      }
      break;
    case C('H'): case '\x7f':  // Backspace
      if(input.f != input.w){
        input.e--;
        input.f--;
        consputc(BACKSPACE);
      }
      break;
    case KEY_LF: // Key Left
    {
      if(input.f != input.w)
      {
        input.f--;
        cgaputc(KEY_LF);
        
      }

      break;
    }
    case KEY_RT: // Key Right
    {
      int IsCanRight = input.e - input.f;
      if(IsCanRight > 0)
      {
        input.f++;
        cgaputc(KEY_RT);
        
      }

      break;
    }
    case KEY_UP: // Key Up
    {
      int NextHistoryPos = (HistoryPos + MAX_HISTORY_LENGTH - 1) % MAX_HISTORY_LENGTH;
      if(NextHistoryPos < MaxHistoryPos)
      {
        HistoryPos = NextHistoryPos;
        SelectFromHistory();
      }
      
      break;
    }
    case KEY_DN: // Key Down
    {
      if(HistoryPos < MaxHistoryPos)
      {
        HistoryPos = (HistoryPos + 1) % MAX_HISTORY_LENGTH;
        SelectFromHistory();
      }
      break;
    }
    default:
      if(c != 0 && input.e-input.r < INPUT_BUF)
      {
        c = (c == '\r') ? '\n' : c;
       
        if(c == '\n' || c == C('D') || input.e == input.r+INPUT_BUF)
        {
          // Insert Command To History
          if(input.e > input.w) // if command exist
          {
            int j;
            for(j = 0; input.w+j<input.e; ++j)
            {
              History[HistoryWrite][j] = input.buf[(input.w +j) % INPUT_BUF];
            }
            History[HistoryWrite][j] = '\0';
            HistoryWrite = (HistoryWrite + 1) % MAX_HISTORY_LENGTH;
            MaxHistoryPos = (MaxHistoryPos == MAX_HISTORY_LENGTH) ? MAX_HISTORY_LENGTH : (MaxHistoryPos + 1);
            HistoryPos = HistoryWrite;
          }  

          input.buf[input.e++ % INPUT_BUF] = c;
          input.f = input.e;

          consputc(c);

          input.w = input.e;

          wakeup(&input.r);
        }
        else
        {
          input.buf[input.f++ % INPUT_BUF] = c;

          if(input.f > input.e)
          { 
            input.e = input.f;
          }

          consputc(c);
        }
      }
      break;
    }
   // cprintf("buf %s, read %d, write %d, edit %d\n final %d\n",input.buf,input.r,input.w,input.e, input.f);
  }
  release(&input.lock);
}

int
consoleread(struct inode *ip, char *dst, int n)
{
  uint target;
  int c;

  iunlock(ip);
  target = n;
  acquire(&input.lock);
  while(n > 0){
    while(input.r == input.w){
      if(proc->killed){
        release(&input.lock);
        ilock(ip);
        return -1;
      }
      sleep(&input.r, &input.lock);
    }
    c = input.buf[input.r++ % INPUT_BUF];
    if(c == C('D')){  // EOF
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        input.r--;
      }
      break;
    }
    *dst++ = c;
    --n;
    if(c == '\n')
      break;
  }
  release(&input.lock);
  ilock(ip);

  return target - n;
}

int
consolewrite(struct inode *ip, char *buf, int n)
{
  int i;

  iunlock(ip);
  acquire(&cons.lock);
  for(i = 0; i < n; i++)
    consputc(buf[i] & 0xff);
  release(&cons.lock);
  ilock(ip);

  return n;
}

void
consoleinit(void)
{
  initlock(&cons.lock, "console");
  initlock(&input.lock, "input");

  devsw[CONSOLE].write = consolewrite;
  devsw[CONSOLE].read = consoleread;
  cons.locking = 1;

  picenable(IRQ_KBD);
  ioapicenable(IRQ_KBD, 0);
}

