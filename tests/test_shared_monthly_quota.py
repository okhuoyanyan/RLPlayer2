"""Shared 250 GB contract through real HTTP and Store accounting interfaces."""
from __future__ import annotations
import json
from pathlib import Path
import sys
import unittest
import concurrent.futures
import time

sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_rlplayer_server import ServiceCase
from rlplayer_service import HTTPFailure
from rlplayer_service import Config, Store
MEASUREMENTS={}

class SharedMonthlyQuotaTests(ServiceCase):
    def test_manifest_after_5_001_GB_API_uses_remaining_total_budget(self):
        self.put_latest()
        self.store.api_response('/api/test-fixture',200,5_001_000_000,'',platform='windows')
        status,_,body=self.request('GET','/api/v1/update/windows/latest')
        self.assertEqual(status,200,'5 GB is not a separate API cutoff; the shared budget is 250 GB')
        self.assertEqual(json.loads(body)['alg'],'PS256')
        # A client can read the final body bytes just before the writer's
        # settlement transaction commits. Observe that public accounting seam.
        deadline=time.monotonic()+2
        while True:
            traffic=self.store.summary()['traffic']
            if traffic['apiBytes']==5_001_000_000+len(body) or time.monotonic()>deadline:break
            time.sleep(0.005)
        self.assertEqual(traffic['apiBytes'],5_001_000_000+len(body))
        self.assertEqual(traffic['totalLimit'],250_000_000_000)
        MEASUREMENTS['API_after_5001_MB']={'HTTP_status':status,'response_body_bytes':len(body),'API_bytes_before':5_001_000_000,'API_bytes_after':traffic['apiBytes'],'total_limit':traffic['totalLimit']}

    def test_manifest_body_cannot_cross_remaining_total_budget(self):
        self.put_latest()
        self.store.api_response('/api/test-fixture',200,249_999_999_999,'',platform='mac')
        status,headers,body=self.request('GET','/api/v1/update/windows/latest')
        self.assertEqual(status,429)
        self.assertEqual(body,b'', 'A rejection body cannot spend an already exhausted budget')
        self.assertEqual(headers['content-length'],'0')
        self.assertIn('retry-after',headers)
        self.assertEqual(self.store.summary()['traffic']['totalBytes'],249_999_999_999)
        MEASUREMENTS['one_byte_remaining_denial']={'HTTP_status':status,'body_bytes':len(body),'metered_total':self.store.summary()['traffic']['totalBytes']}

    def test_objects_include_API_and_both_platforms_in_one_budget(self):
        self.store.api_response('/api/test-fixture',200,10_000_000_000,'',platform='windows')
        reservation=self.store.reserve_object('a'*64,240_000_000_000,platform='mac')
        with self.assertRaises(HTTPFailure) as denied:
            self.store.reserve_object('b'*64,1,platform='windows')
        self.assertEqual(denied.exception.status,429)
        self.assertTrue(self.store.settle_object(reservation,240_000_000_000))
        self.assertEqual(self.store.summary()['traffic']['totalBytes'],250_000_000_000)

    def test_exact_250GB_rejects_API_and_object_HTTP_on_both_platforms(self):
        self.put_latest()
        object_id,_=self.put_object(b'object-body')
        self.store.api_response('/api/test-fixture',200,100_000_000_000,'',platform='windows')
        reserved=self.store.reserve_object('a'*64,150_000_000_000,platform='mac')
        self.store.settle_object(reserved,150_000_000_000)
        for route in ('windows/latest','macos/latest',f'windows/objects/{object_id}.zip',f'macos/objects/{object_id}.zip'):
            status,headers,body=self.request('GET','/api/v1/update/'+route)
            self.assertEqual(status,429)
            self.assertEqual(body,b'')
            self.assertGreater(int(headers['retry-after']),0)
        self.assertEqual(self.store.summary()['traffic']['totalCommitted'],250_000_000_000)
        MEASUREMENTS['exact_total_limit']={'tested_HTTP_paths':4,'denied':4,'status':429,'body_bytes':0,'committed_total':self.store.summary()['traffic']['totalCommitted']}

    def test_API_HTTP_concurrency_cannot_spend_the_same_final_response_budget(self):
        self.put_latest()
        body_size=self.store.latest.stat().st_size
        seed=250_000_000_000-body_size
        self.store.api_response('/api/test-fixture',200,seed,'',platform='mac')
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
            results=list(executor.map(lambda _:self.request('GET','/api/v1/update/windows/latest'),range(12)))
        statuses=[r[0] for r in results]
        self.assertEqual(statuses.count(200),1,statuses)
        self.assertEqual(statuses.count(429),11,statuses)
        deadline=time.monotonic()+2
        while True:
            traffic=self.store.summary()['traffic']
            if traffic['totalBytes']==250_000_000_000 or time.monotonic()>deadline:break
            time.sleep(0.005)
        self.assertEqual(traffic['totalBytes'],250_000_000_000)
        self.assertEqual(traffic['apiReserved'],0)
        MEASUREMENTS['HTTP_API_concurrency']={'requests':len(results),'successes':statuses.count(200),'denials':statuses.count(429),'response_body_bytes':body_size,'final_total':traffic['totalBytes'],'active_API_reservation_bytes':traffic['apiReserved']}

    def test_XAccel_partial_settlement_then_range_resume_uses_actual_object_bytes(self):
        object_id,_=self.put_object(b'x'*100)
        self.store.config.x_accel=True
        self.store.config.accounting_token='fixture-accounting'
        self.store.config.monthly_object_bytes=99
        self.store.config.monthly_api_bytes=1
        route=f'/api/v1/update/windows/objects/{object_id}.zip'
        status,headers,_=self.request('GET',route)
        self.assertEqual(status,200)
        reservation=headers['x-rlplayer-reservation']
        status,_,body=self.json_request('POST',f'/internal/v1/account?reservation={reservation}&actual=7',{}, {'X-Accounting-Token':'fixture-accounting'})
        self.assertEqual(status,200)
        self.assertTrue(body['settled'])
        self.assertEqual(self.store.summary()['traffic']['objectBytes'],7)
        status,headers,_=self.request('GET',route,headers={'Range':'bytes=7-99'})
        self.assertEqual(status,206)
        reservation=headers['x-rlplayer-reservation']
        self.assertEqual(headers['content-range'],'bytes 7-99/100')
        self.json_request('POST',f'/internal/v1/account?reservation={reservation}&actual=93',{}, {'X-Accounting-Token':'fixture-accounting'})
        self.assertEqual(self.store.summary()['traffic']['totalCommitted'],100)
        MEASUREMENTS['XAccel_interrupt_resume']={'first_actual_object_bytes':7,'resumed_actual_object_bytes':93,'final_total':self.store.summary()['traffic']['totalCommitted']}

    def test_legacy_configuration_and_existing_counters_survive_reopen(self):
        self.store.api_response('/api/test-fixture',200,5_001_000_000,'',platform='mac')
        config=Config.load({'monthly_object_bytes':245_000_000_000,'monthly_api_bytes':5_000_000_000})
        reopened=Store(self.root,config)
        self.assertEqual(reopened.summary()['traffic']['apiBytes'],5_001_000_000)
        self.assertEqual(config.monthly_total_bytes,250_000_000_000)
        self.assertEqual(config.storage_bytes,5*1024**3)
        self.assertEqual(config.windows_update_bytes,5*1024**3)
        self.assertEqual(config.macos_update_bytes,5*1024**3)
        MEASUREMENTS['legacy_config_and_persistence']={'retained_API_bytes':reopened.summary()['traffic']['apiBytes'],'monthly_total':config.monthly_total_bytes,'storage_bytes':config.storage_bytes,'windows_storage_bytes':config.windows_update_bytes,'macos_storage_bytes':config.macos_update_bytes}

    def test_shared_summary_and_overview_do_not_report_API_5GB_exhausted(self):
        self.store.api_response('/api/test-fixture',200,5_001_000_000,'',platform='mac')
        for traffic in (self.store.summary()['traffic'],self.store.overview()['traffic']):
            self.assertEqual(traffic['totalLimit'],250_000_000_000)
            self.assertEqual(traffic['totalBytes'],5_001_000_000)
            self.assertFalse(traffic['warning80'])
            self.assertFalse(traffic['incrementalOnly90'])
            self.assertAlmostEqual(traffic['ratio'],0.020004)

    def test_API_reservation_settlement_and_expiration_stay_in_API_column(self):
        reservation=self.store.reserve_api(300,platform='mac')
        pending_traffic=self.store.summary()['traffic']
        self.assertEqual(pending_traffic['objectReserved'],0)
        self.assertEqual(pending_traffic['apiReserved'],300)
        self.assertTrue(self.store.settle_object(reservation,123))
        self.assertFalse(self.store.settle_object(reservation,300))
        self.assertEqual(self.store.summary()['traffic']['objectBytes'],0)
        self.assertEqual(self.store.summary()['traffic']['apiBytes'],123)
        pending=self.store.reserve_api(200,platform='windows')
        # Clock injection at the system boundary, not an internal function mock.
        self.store.config.reservation_ttl=0.001
        time.sleep(0.01)
        traffic=self.store.summary()['traffic']
        self.assertEqual((traffic['objectBytes'],traffic['apiBytes']),(0,323))
        self.assertFalse(self.store.settle_object(pending,0))
        MEASUREMENTS['API_expiration_classification']={'settled_API_bytes':123,'expired_API_bytes':200,'final_API_bytes':traffic['apiBytes'],'final_object_bytes':traffic['objectBytes']}

    def test_concurrent_API_and_object_reservations_share_one_atomic_budget(self):
        self.store.api_response('/api/test-fixture',200,249_999_999_900,'',platform='windows')
        def reserve(i):
            try:
                if i%2:return self.store.reserve_api(10,platform='mac')
                return self.store.reserve_object(f'{i:064x}',10,platform='windows')
            except HTTPFailure as failure:
                self.assertEqual(failure.status,429)
                return None
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as executor:
            admitted=list(executor.map(reserve,range(40)))
        accepted=[r for r in admitted if r]
        self.assertEqual(len(accepted),10)
        summary=self.store.summary()
        self.assertEqual(summary['reservations']['bytes'],100)
        self.assertEqual(summary['traffic']['totalBytes'],249_999_999_900)
        for reservation in accepted:self.assertTrue(self.store.settle_object(reservation,10))
        self.assertEqual(self.store.summary()['traffic']['totalBytes'],250_000_000_000)
        MEASUREMENTS['mixed_reservation_concurrency']={'requests':len(admitted),'admitted':len(accepted),'denied':len(admitted)-len(accepted),'reservation_bytes_each':10,'reserved_bytes':summary['reservations']['bytes'],'final_total':self.store.summary()['traffic']['totalBytes']}

if __name__=='__main__':unittest.main()
