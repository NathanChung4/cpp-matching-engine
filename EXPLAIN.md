# How an order book matches orders, for someone new to it

This page explains what this project does without assuming you know anything about
exchanges. If you want to see it happen while you read, build the project and run the
walkthrough; it prints the book after every single order:

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/release
build/release/me ladder scenarios/walkthrough.txt
```

## The order book

An exchange keeps, for each thing it trades (here, one made-up stock), a list of everyone
who wants to buy and everyone who wants to sell, and at what price. That list is the
**order book**.

- A **bid** is an offer to buy: "I will buy 15 shares at $100.00 or less."
- An **ask** (or offer) is an offer to sell: "I will sell 10 shares at $100.05 or more."

The highest bid is the **best bid**. The lowest ask is the **best ask**. The gap between
them is the **spread**. While the best bid is below the best ask, nobody agrees on a
price, so nothing trades and the orders wait in the book.

The tool prints the book as a **price ladder**: asks on top (highest price first), the
spread in the middle, bids below (best first). Each row is one price level: the price, the
total quantity waiting there, and the individual orders in the order they arrived:

```
      ASK   100.07 |        20 | #3:20
      ASK   100.05 |        15 | #1:10 #2:5
      -------------- spread 0.05 --------------
      BID   100.00 |        15 | #4:15
```

Here two people want to sell at $100.05: order #1 (10 shares) arrived before order #2
(5 shares). One person wants to buy 15 at $100.00. The spread is 5 cents.

## Orders

Every order has an id (chosen by whoever sends it), a side (buy or sell), a quantity and,
for most types, a limit price. This engine knows six kinds of instruction:

- A **limit order** says "buy (or sell) up to this many, at this price or better". If it
  can trade right away it does; whatever is left waits in the book at its price.
- A **market order** says "buy (or sell) this many at whatever price is available now".
  It never waits: what cannot fill right away is dropped (the tool prints `EXPIRED`).
- An **IOC** (immediate or cancel) order is a limit order that never waits: it trades
  what it can at its price or better, and the rest expires.
- A **FOK** (fill or kill) order is all or nothing: if the whole quantity can trade right
  now at its price or better, it does; otherwise nothing happens at all and it is
  rejected.
- A **cancel** removes a waiting order from the book.
- A **modify** changes a waiting order's price or quantity. Here it works as cancel and
  replace: the order leaves its place in line and joins the back of the line at its new
  price, even if only the quantity went down.

## Matching: who trades with whom

When a new order arrives that can trade (a buy at or above the best ask, or a sell at or
below the best bid), the engine matches it. The order that just arrived is the **taker**
(it takes liquidity). The waiting order it trades with is the **maker** (it made the
liquidity by waiting in the book).

The rule that decides which waiting order trades first is **price-time priority**:

1. **Price first.** A buyer is matched with the cheapest ask first; a seller with the
   highest bid first. Better prices always go first.
2. **Then time.** Among orders at the same price, the one that arrived first trades
   first. This is first in, first out, and it is why the ladder lists orders in arrival
   order.

The trade happens at the **maker's price**, the price that was waiting in the book. If you
send a buy at $100.10 and the best ask is $100.05, you pay $100.05.

### A worked example

Start from the ladder above and send `limit buy 5 100.05 12` (buy 12 at $100.05 or less):

1. The best ask is $100.05, which is not more than $100.05, so it trades.
2. At $100.05, order #1 arrived first: the buyer takes all 10 of its shares.
3. The buyer still wants 2. Order #2 is next in line at $100.05: it sells 2 of its 5.
4. The buyer is done. Order #2 still has 3 shares waiting, in the same place in line.

The tool prints:

```
[5] limit buy 5 100.05 12
    TRADE 10 @ 100.05  taker #5  maker #1
    TRADE 2 @ 100.05  taker #5  maker #2
      ASK   100.07 |        20 | #3:20
      ASK   100.05 |         3 | #2:3
      -------------- spread 0.05 --------------
      BID   100.00 |        15 | #4:15
```

Order #2 was a **partial fill**: some of it traded, the rest keeps waiting. If an incoming
order is bigger than everything at the best price, it keeps going to the next price; this
is called **sweeping** the book, and the walkthrough shows it with order #6.

## What can go wrong, and what the engine does

- Cancelling an order that already traded in full, or that was never there, is rejected
  with `unknown order id`. In the walkthrough, `cancel 1` comes after order #1 filled.
- Reusing an id that an accepted order already used is rejected (`duplicate order id`).
- A quantity of zero, or a price outside $0.01 to $1,310.71, is rejected.
- A FOK that cannot fill in full is rejected and leaves the book exactly as it was.

None of these stop the engine: a rejection is just another event it reports. The tool
itself only stops on input it cannot read at all (a misspelled command, a bad number, a
missing file, or a cancel of an id the scenario never sent); then it prints one line that
starts with `error:` and says which line of the file is wrong.

## How do we know the engine is right?

A fast order book is fiddly: linked lists, bitmaps, index tables. Bugs hide easily. So
the project also contains a **reference matcher**: a second, deliberately slow order book
written to be obviously correct. It keeps every waiting order in one plain list, and for
every trade it looks through the whole list for the best price and, at that price, the
earliest arrival. You can check it by reading it.

The **differential test** sends the same millions of random orders (limits, markets,
IOCs, FOKs, cancels, modifies and some invalid ones) to both books and demands that they
report exactly the same events, one by one, and end with exactly the same book. Run it
yourself:

```
build/release/me difftest
```

## How fast is it?

Run `./bench`. It builds an optimized version, generates a million random orders from a
fixed seed (so everyone gets the same orders), runs them through a fresh book several
times, and reports orders per second and how long single orders take (the median, the
99th and the 99.9th percentile, so you see the slow tail too, not just the average). It
prints the machine, the compiler and the exact flags, because a speed number means
nothing without them. The README explains how to read the numbers and what they do not
tell you.

## Words used here

- **order book**: all waiting buy and sell orders for one stock.
- **bid / ask**: a waiting buy / sell order.
- **best bid / best ask**: the highest bid / lowest ask.
- **spread**: best ask minus best bid.
- **price level**: all waiting orders at one price on one side.
- **maker / taker**: the waiting order / the order that arrived and traded with it.
- **price-time priority**: better price first, then earlier arrival first.
- **partial fill**: an order that traded only part of its quantity.
- **sweep**: one order trading through several price levels.
- **IOC / FOK**: immediate-or-cancel / fill-or-kill.
- **tick**: the smallest price step, here one cent.
