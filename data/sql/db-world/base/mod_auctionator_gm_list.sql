--
-- GM curated listing list for mod-auctionator.
--
-- Rows of this table are listed by ".auctionator addlist [house] [owner]".
-- Price is the UNIT price in copper; the buyout becomes price * stack.
-- owner = 0 means the configured auctionator character, i.e. the sale money is
-- recycled by the system (gold sink) - see mod_auctionator.conf.dist.
--

--
-- This file lives in `base/`, but the AzerothCore updater registers every
-- `modules/<mod>/data/sql/db-<db>/` directory (base included) as a MODULE update and
-- RE-RUNS the whole file whenever its hash changes. It therefore has to stay
-- idempotent: CREATE TABLE IF NOT EXISTS + INSERT IGNORE, so the rows a GM curated
-- (or enabled) are never dropped or overwritten.
--

CREATE TABLE IF NOT EXISTS `mod_auctionator_gm_list` (
  `item` int unsigned NOT NULL COMMENT 'item_template.entry',
  `price` int unsigned NOT NULL DEFAULT '0' COMMENT 'unit price in copper; buyout = price * stack',
  `stack` int unsigned NOT NULL DEFAULT '1' COMMENT 'stack size, capped by the item max stack',
  `hours` int unsigned NOT NULL DEFAULT '48' COMMENT 'auction duration in hours (1..720)',
  `house` int unsigned NOT NULL DEFAULT '7' COMMENT '2 = alliance, 6 = horde, 7 = neutral',
  `owner` int unsigned NOT NULL DEFAULT '0' COMMENT 'character guid receiving the sale money; 0 = the configured auctionator character (gold recycled)',
  `enabled` tinyint NOT NULL DEFAULT '1' COMMENT '0 to keep the row in the table but skip it',
  PRIMARY KEY (`item`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

--
-- Example rows (kept disabled so a fresh install lists nothing by accident).
-- Adjust the prices for your realm and set enabled = 1.
--
INSERT IGNORE INTO `mod_auctionator_gm_list` (`item`, `price`, `stack`, `hours`, `house`, `owner`, `enabled`) VALUES
(5500, 10000, 1, 48, 7, 0, 0),   -- Iridescent Pearl
(4359, 500, 5, 48, 7, 0, 0);     -- Handful of Copper Bolts
