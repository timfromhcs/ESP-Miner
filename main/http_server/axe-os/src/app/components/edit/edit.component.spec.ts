import { ComponentFixture, TestBed } from '@angular/core/testing';

import { EditComponent } from './edit.component';
import { provideHttpClient } from '@angular/common/http';
import { provideToastr } from 'ngx-toastr';
import { provideRouter } from '@angular/router';
import { FormControl, FormGroup } from '@angular/forms';
import { of } from 'rxjs';

import { SystemApiService } from '../../services/system.service';

describe('EditComponent', () => {
  let component: EditComponent;
  let fixture: ComponentFixture<EditComponent>;

  beforeEach(() => {
    TestBed.configureTestingModule({
      imports: [EditComponent],
      providers: [provideHttpClient(), provideToastr(), provideRouter([])]
    });
    fixture = TestBed.createComponent(EditComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  it('should not offer the fixed ST7789 display as a selectable option', () => {
    expect(component.displays).not.toContain('ST7789 (320x170)');
  });

  it('should treat a fixed ST7789 display as non-configurable', () => {
    component.form = new FormGroup({
      display: new FormControl('ST7789 (320x170)')
    });

    expect(component.isDisplayConfigurable).toBeFalse();
  });

  describe('advanced tuning toggles', () => {
    let updates: any[];
    let systemApi: jasmine.SpyObj<SystemApiService>;

    beforeEach(() => {
      updates = [];
      systemApi = jasmine.createSpyObj<SystemApiService>('SystemApiService', ['updateSystem']);
      systemApi.updateSystem.and.callFake((_uri: string, body: any) => {
        updates.push(body);
        return of({});
      });
      // The component is already instantiated by the outer beforeEach, so the
      // service is injected directly instead of via TestBed.overrideProvider.
      component['systemService'] = systemApi;
    });

    it('defaults both advanced options to off', () => {
      expect(component.asicFastUart).toBeFalse();
      expect(component.autotuneVoltage).toBeFalse();
    });

    it('enables the fast ASIC UART and tells the user a restart is needed', () => {
      component.toggleAsicFastUart();

      expect(updates).toEqual([{ asicFastUart: 1 }]);
      expect(component.asicFastUart).toBeTrue();
      expect(component.fastUartSaving).toBeFalse();
    });

    it('disables the fast ASIC UART again', () => {
      component.asicFastUart = true;

      component.toggleAsicFastUart();

      expect(updates).toEqual([{ asicFastUart: 0 }]);
      expect(component.asicFastUart).toBeFalse();
    });

    it('starts and stops the voltage autotuner', () => {
      component.toggleAutotuneVoltage();
      expect(updates).toEqual([{ autotuneVoltage: 1 }]);
      expect(component.autotuneVoltage).toBeTrue();

      component.toggleAutotuneVoltage();
      expect(updates[1]).toEqual({ autotuneVoltage: 0 });
      expect(component.autotuneVoltage).toBeFalse();
    });
  });
});
